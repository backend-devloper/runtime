/*
 * branch-opts.c: Branch optimizations support 
 *
 * Authors:
 *   Patrik Torstensson (Patrik.Torstesson at gmail.com)
 *
 * (C) 2005 Ximian, Inc.  http://www.ximian.com
 */
 #include "mini.h"
 
 /*
 * Used by the arch code to replace the exception handling
 * with a direct branch. This is safe to do if the 
 * exception object isn't used, no rethrow statement and
 * no filter statement (verify).
 *
 */
MonoInst *
mono_branch_optimize_exception_target (MonoCompile *cfg, MonoBasicBlock *bb, const char * exname)
{
	MonoMethod *method = cfg->method;
	MonoMethodHeader *header = mono_method_get_header (method);
	MonoExceptionClause *clause;
	MonoClass *exclass;
	int i;

	if (!(cfg->opt & MONO_OPT_EXCEPTION))
		return NULL;

	if (bb->region == -1 || !MONO_BBLOCK_IS_IN_REGION (bb, MONO_REGION_TRY))
		return NULL;

	exclass = mono_class_from_name (mono_get_corlib (), "System", exname);
	/* search for the handler */
	for (i = 0; i < header->num_clauses; ++i) {
		clause = &header->clauses [i];
		if (MONO_OFFSET_IN_CLAUSE (clause, bb->real_offset)) {
			if (clause->flags == MONO_EXCEPTION_CLAUSE_NONE && clause->data.catch_class && mono_class_is_assignable_from (clause->data.catch_class, exclass)) {
				MonoBasicBlock *tbb;

				/* get the basic block for the handler and 
				 * check if the exception object is used.
				 * Flag is set during method_to_ir due to 
				 * pop-op is optmized away in codegen (burg).
				 */
				tbb = cfg->cil_offset_to_bb [clause->handler_offset];
				if (tbb && tbb->flags & BB_EXCEPTION_DEAD_OBJ && !(tbb->flags & BB_EXCEPTION_UNSAFE)) {
					MonoBasicBlock *targetbb = tbb;
					gboolean unsafe = FALSE;

					/* Check if this catch clause is ok to optimize by
					 * looking for the BB_EXCEPTION_UNSAFE in every BB that
					 * belongs to the same region. 
					 *
					 * UNSAFE flag is set during method_to_ir (OP_RETHROW)
					 */
					while (!unsafe && tbb->next_bb && tbb->region == tbb->next_bb->region) {
						if (tbb->next_bb->flags & BB_EXCEPTION_UNSAFE)  {
							unsafe = TRUE;
							break;
						}
						tbb = tbb->next_bb;
					}

					if (!unsafe) {
						MonoInst *jump;

						/* Create dummy inst to allow easier integration in
						 * arch dependent code (opcode ignored)
						 */
						MONO_INST_NEW (cfg, jump, OP_BR);

						/* Allocate memory for our branch target */
						jump->inst_i1 = mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst));
						jump->inst_true_bb = targetbb;

						if (cfg->verbose_level > 2) 
							g_print ("found exception to optimize - returning branch to BB%d (%s) (instead of throw) for method %s:%s\n", targetbb->block_num, clause->data.catch_class->name, cfg->method->klass->name, cfg->method->name);

						return jump;
					} 

					return NULL;
				}
			} else {
				/* Branching to an outer clause could skip inner clauses */
				return NULL;
			}
		}
	}

	return NULL;
}

static const int int_cmov_opcodes [] = {
	OP_CMOV_IEQ,
	OP_CMOV_INE_UN,
	OP_CMOV_ILE,
	OP_CMOV_IGE,
	OP_CMOV_ILT,
	OP_CMOV_IGT,
	OP_CMOV_ILE_UN,
	OP_CMOV_IGE_UN,
	OP_CMOV_ILT_UN,
	OP_CMOV_IGT_UN
};

static const int long_cmov_opcodes [] = {
	OP_CMOV_LEQ,
	OP_CMOV_LNE_UN,
	OP_CMOV_LLE,
	OP_CMOV_LGE,
	OP_CMOV_LLT,
	OP_CMOV_LGT,
	OP_CMOV_LLE_UN,
	OP_CMOV_LGE_UN,
	OP_CMOV_LLT_UN,
	OP_CMOV_LGT_UN
};

void
mono_if_conversion (MonoCompile *cfg)
{
#ifdef MONO_ARCH_HAVE_CMOV_OPS
	MonoBasicBlock *bb;
	gboolean changed = FALSE;

	if (!(cfg->opt & MONO_OPT_CMOV))
		return;

	// FIXME: Make this work with extended bblocks

	/* 
	 * This pass requires somewhat optimized IR code so it should be run after
	 * local cprop/deadce. Also, it should be run before dominator computation, since
	 * it changes control flow.
	 */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoBasicBlock *bb1, *bb2;

	restart:
		/* Look for the IR code generated from cond ? a : b
		 * which is:
		 * BB:
		 * b<cond> [BB1BB2]
		 * BB1:
		 * <var> <- <a>
		 * br BB3
		 * BB2:
		 * <var> <- <b>
		 * br BB3
		 */
		if (!(bb->out_count == 2 && !bb->extended))
			continue;

		bb1 = bb->out_bb [0];
		bb2 = bb->out_bb [1];

		if (bb1->in_count == 1 && bb2->in_count == 1 && bb1->out_count == 1 && bb2->out_count == 1 && bb1->out_bb [0] == bb2->out_bb [0]) {
			MonoInst *prev, *compare, *branch, *ins1, *ins2, *cmov, *move, *tmp;
			gboolean simple, ret;
			int dreg, tmp_reg;
			CompType comp_type;

			/* 
			 * Check that bb1 and bb2 are 'simple' and both assign to the same
			 * variable.
			 */
			/* FIXME: Get rid of the nops earlier */
			ins1 = bb1->code;
			while (ins1 && ins1->opcode == OP_NOP)
				ins1 = ins1->next;
			ins2 = bb2->code;
			while (ins2 && ins2->opcode == OP_NOP)
				ins2 = ins2->next;
			if (!(ins1 && ins2 && ins1->dreg == ins2->dreg && ins1->dreg != -1))
				continue;

			simple = TRUE;
			for (tmp = ins1->next; tmp; tmp = tmp->next)
				if (!((tmp->opcode == OP_NOP) || (tmp->opcode == OP_BR)))
					simple = FALSE;
					
			for (tmp = ins2->next; tmp; tmp = tmp->next)
				if (!((tmp->opcode == OP_NOP) || (tmp->opcode == OP_BR)))
					simple = FALSE;

			if (!simple)
				continue;

			/* We move ins1/ins2 before the compare so they should have no side effect */
			if (!(MONO_INS_HAS_NO_SIDE_EFFECT (ins1) && MONO_INS_HAS_NO_SIDE_EFFECT (ins2)))
				continue;

			if (bb->last_ins && (bb->last_ins->opcode == OP_BR_REG || bb->last_ins->opcode == OP_BR))
				continue;

			/* Find the compare instruction */
			/* FIXME: Optimize this using prev */
			prev = NULL;
			compare = bb->code;
			g_assert (compare);
			while (compare->next && !MONO_IS_COND_BRANCH_OP (compare->next)) {
				prev = compare;
				compare = compare->next;
			}
			g_assert (compare->next && MONO_IS_COND_BRANCH_OP (compare->next));
			branch = compare->next;

			/* Moving ins1/ins2 could change the comparison */
			/* FIXME: */
			if (!((compare->sreg1 != ins1->dreg) && (compare->sreg2 != ins1->dreg)))
				continue;

			/* FIXME: */
			comp_type = mono_opcode_to_type (branch->opcode, compare->opcode);
			if (!((comp_type == CMP_TYPE_I) || (comp_type == CMP_TYPE_L)))
				continue;

			/* FIXME: */
			/* ins->type might not be set */
			if (INS_INFO (ins1->opcode) [MONO_INST_DEST] != 'i')
				continue;

			if (cfg->verbose_level > 2) {
				printf ("\tBranch -> CMove optimization in BB%d on\n", bb->block_num);
				printf ("\t\t"); mono_print_ins (compare);
				printf ("\t\t"); mono_print_ins (compare->next);
				printf ("\t\t"); mono_print_ins (ins1);
				printf ("\t\t"); mono_print_ins (ins2);
			}

			changed = TRUE;

			//printf ("HIT!\n");

			/* Assignments to the return register must remain at the end of bbs */
			if (cfg->ret)
				ret = ins1->dreg == cfg->ret->dreg;
			else
				ret = FALSE;

			tmp_reg = mono_alloc_dreg (cfg, STACK_I4);
			dreg = ins1->dreg;

			/* Rewrite ins1 to emit to tmp_reg */
			ins1->dreg = tmp_reg;

			if (ret) {
				dreg = mono_alloc_dreg (cfg, STACK_I4);
				ins2->dreg = dreg;
			}

			/* Remove ins1/ins2 from bb1/bb2 */
			MONO_REMOVE_INS (bb1, ins1);
			MONO_REMOVE_INS (bb2, ins2);

			/* Move ins1 and ins2 before the comparison */
			/* ins1 comes first to avoid ins1 overwriting an argument of ins2 */
			mono_bblock_insert_before_ins (bb, compare, ins2);
			mono_bblock_insert_before_ins (bb, ins2, ins1);

			/* Add cmov instruction */
			MONO_INST_NEW (cfg, cmov, OP_NOP);
			cmov->dreg = dreg;
			cmov->sreg1 = dreg;
			cmov->sreg2 = tmp_reg;
			switch (mono_opcode_to_type (branch->opcode, compare->opcode)) {
			case CMP_TYPE_I:
				cmov->opcode = int_cmov_opcodes [mono_opcode_to_cond (branch->opcode)];
				break;
			case CMP_TYPE_L:
				cmov->opcode = long_cmov_opcodes [mono_opcode_to_cond (branch->opcode)];
				break;
			default:
				g_assert_not_reached ();
			}
			mono_bblock_insert_after_ins (bb, compare, cmov);

			if (ret) {
				/* Add an extra move */
				MONO_INST_NEW (cfg, move, OP_MOVE);
				move->dreg = cfg->ret->dreg;
				move->sreg1 = dreg;
				mono_bblock_insert_after_ins (bb, cmov, move);
			}

			/* Rewrite the branch */
			branch->opcode = OP_BR;
			branch->inst_target_bb = bb1->out_bb [0];
			mono_link_bblock (cfg, bb, branch->inst_target_bb);

			/* Reorder bblocks */
			mono_unlink_bblock (cfg, bb, bb1);
			mono_unlink_bblock (cfg, bb, bb2);
			mono_unlink_bblock (cfg, bb1, bb1->out_bb [0]);
			mono_unlink_bblock (cfg, bb2, bb2->out_bb [0]);
			mono_remove_bblock (cfg, bb1);
			mono_remove_bblock (cfg, bb2);

			/* Merge bb and its successor if possible */
			if ((bb->out_bb [0]->in_count == 1) && (bb->out_bb [0] != cfg->bb_exit) &&
				(bb->region == bb->out_bb [0]->region)) {
				mono_merge_basic_blocks (cfg, bb, bb->out_bb [0]);
				goto restart;
			}
		}

		/* Look for the IR code generated from if (cond) <var> <- <a>
		 * which is:
		 * BB:
		 * b<cond> [BB1BB2]
		 * BB1:
		 * <var> <- <a>
		 * br BB2
		 */

		if ((bb2->in_count == 1 && bb2->out_count == 1 && bb2->out_bb [0] == bb1) ||
			(bb1->in_count == 1 && bb1->out_count == 1 && bb1->out_bb [0] == bb2)) {
			MonoInst *prev, *compare, *branch, *ins1, *cmov, *tmp;
			gboolean simple;
			int dreg, tmp_reg;
			CompType comp_type;
			CompRelation cond;
			MonoBasicBlock *next_bb, *code_bb;

			/* code_bb is the bblock containing code, next_bb is the successor bblock */
			if (bb2->in_count == 1 && bb2->out_count == 1 && bb2->out_bb [0] == bb1) {
				code_bb = bb2;
				next_bb = bb1;
			} else {
				code_bb = bb1;
				next_bb = bb2;
			}

			ins1 = code_bb->code;

			if (!ins1)
				continue;

			/* Check that code_bb is simple */
			simple = TRUE;
			for (tmp = ins1->next; tmp; tmp = tmp->next)
				if (!((tmp->opcode == OP_NOP) || (tmp->opcode == OP_BR)))
					simple = FALSE;

			if (!simple)
				continue;

			/* We move ins1 before the compare so it should have no side effect */
			if (!MONO_INS_HAS_NO_SIDE_EFFECT (ins1))
				continue;

			if (bb->last_ins && bb->last_ins->opcode == OP_BR_REG)
				continue;

			/* Find the compare instruction */
			/* FIXME: Optimize this using prev */
			prev = NULL;
			compare = bb->code;
			g_assert (compare);
			while (compare->next && !MONO_IS_COND_BRANCH_OP (compare->next)) {
				prev = compare;
				compare = compare->next;
			}
			g_assert (compare->next && MONO_IS_COND_BRANCH_OP (compare->next));
			branch = compare->next;

			/* FIXME: */
			comp_type = mono_opcode_to_type (branch->opcode, compare->opcode);
			if (!((comp_type == CMP_TYPE_I) || (comp_type == CMP_TYPE_L)))
				continue;

			/* FIXME: */
			/* ins->type might not be set */
			if (INS_INFO (ins1->opcode) [MONO_INST_DEST] != 'i')
				continue;

			/* FIXME: */
			if (cfg->ret && ins1->dreg == cfg->ret->dreg)
				continue;

			if (cfg->verbose_level > 2) {
				printf ("\tBranch -> CMove optimization (2) in BB%d on\n", bb->block_num);
				printf ("\t\t"); mono_print_ins (compare);
				printf ("\t\t"); mono_print_ins (compare->next);
				printf ("\t\t"); mono_print_ins (ins1);
			}

			changed = TRUE;

			//printf ("HIT!\n");

			tmp_reg = mono_alloc_dreg (cfg, STACK_I4);
			dreg = ins1->dreg;

			/* Rewrite ins1 to emit to tmp_reg */
			ins1->dreg = tmp_reg;

			/* Remove ins1 from code_bb */
			MONO_REMOVE_INS (code_bb, ins1);

			/* Move ins1 before the comparison */
			mono_bblock_insert_before_ins (bb, compare, ins1);

			/* Add cmov instruction */
			MONO_INST_NEW (cfg, cmov, OP_NOP);
			cmov->dreg = dreg;
			cmov->sreg1 = dreg;
			cmov->sreg2 = tmp_reg;
			cond = mono_opcode_to_cond (branch->opcode);
			if (branch->inst_false_bb == code_bb)
				cond = mono_negate_cond (cond);
			switch (mono_opcode_to_type (branch->opcode, compare->opcode)) {
			case CMP_TYPE_I:
				cmov->opcode = int_cmov_opcodes [cond];
				break;
			case CMP_TYPE_L:
				cmov->opcode = long_cmov_opcodes [cond];
				break;
			default:
				g_assert_not_reached ();
			}
			mono_bblock_insert_after_ins (bb, compare, cmov);

			/* Rewrite the branch */
			branch->opcode = OP_BR;
			branch->inst_target_bb = next_bb;
			mono_link_bblock (cfg, bb, branch->inst_target_bb);

			/* Nullify the branch at the end of code_bb */
			if (code_bb->code) {
				branch = code_bb->code;
				MONO_DELETE_INS (code_bb, branch);
			}

			/* Reorder bblocks */
			mono_unlink_bblock (cfg, bb, code_bb);
			mono_unlink_bblock (cfg, code_bb, next_bb);

			/* Merge bb and its successor if possible */
			if ((bb->out_bb [0]->in_count == 1) && (bb->out_bb [0] != cfg->bb_exit) &&
				(bb->region == bb->out_bb [0]->region)) {
				mono_merge_basic_blocks (cfg, bb, bb->out_bb [0]);
				goto restart;
			}
		}
	}

#if 0
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoBasicBlock *bb1, *bb2;
		MonoInst *prev, *compare, *branch, *ins1, *ins2, *cmov, *move, *tmp;
		gboolean simple, ret;
		int dreg, tmp_reg;
		CompType comp_type;

		/* Look for the IR code generated from if (cond) <var> <- <a>
		 * after branch opts which is:
		 * BB:
		 * compare
		 * b<cond> [BB1]
		 * <var> <- <a>
		 * BB1:
		 */
		if (!(bb->out_count == 1 && bb->extended && bb->code && bb->code->next && bb->code->next->next))
			continue;

		mono_print_bb (bb, "");

		/* Find the compare instruction */
		prev = NULL;
		compare = bb->code;
		g_assert (compare);
		while (compare->next->next && compare->next->next != bb->last_ins) {
			prev = compare;
			compare = compare->next;
		}
		branch = compare->next;
		if (!MONO_IS_COND_BRANCH_OP (branch))
			continue;
	}
#endif

	if (changed) {
		if (cfg->opt & MONO_OPT_BRANCH)
			mono_optimize_branches (cfg);
		/* Merging bblocks could make some variables local */
		mono_handle_global_vregs (cfg);
		if (cfg->opt & (MONO_OPT_CONSPROP | MONO_OPT_COPYPROP))
			mono_local_cprop2 (cfg);
		mono_local_deadce (cfg);
	}
#endif
}

void
mono_nullify_basic_block (MonoBasicBlock *bb) 
{
	bb->in_count = 0;
	bb->out_count = 0;
	bb->in_bb = NULL;
	bb->out_bb = NULL;
	bb->next_bb = NULL;
	bb->code = bb->last_ins = NULL;
	bb->cil_code = NULL;
}

static void 
replace_out_block (MonoBasicBlock *bb, MonoBasicBlock *orig,  MonoBasicBlock *repl)
{
	int i;

	for (i = 0; i < bb->out_count; i++) {
		MonoBasicBlock *ob = bb->out_bb [i];
		if (ob == orig) {
			if (!repl) {
				if (bb->out_count > 1) {
					bb->out_bb [i] = bb->out_bb [bb->out_count - 1];
				}
				bb->out_count--;
			} else {
				bb->out_bb [i] = repl;
			}
		}
	}
}

static void 
replace_in_block (MonoBasicBlock *bb, MonoBasicBlock *orig, MonoBasicBlock *repl)
{
	int i;

	for (i = 0; i < bb->in_count; i++) {
		MonoBasicBlock *ib = bb->in_bb [i];
		if (ib == orig) {
			if (!repl) {
				if (bb->in_count > 1) {
					bb->in_bb [i] = bb->in_bb [bb->in_count - 1];
				}
				bb->in_count--;
			} else {
				bb->in_bb [i] = repl;
			}
		}
	}
}

static void
replace_out_block_in_code (MonoBasicBlock *bb, MonoBasicBlock *orig, MonoBasicBlock *repl) {
	MonoInst *ins;
	
	for (ins = bb->code; ins != NULL; ins = ins->next) {
		switch (ins->opcode) {
		case OP_BR:
			if (ins->inst_target_bb == orig)
				ins->inst_target_bb = repl;
			break;
		case OP_CALL_HANDLER:
			if (ins->inst_target_bb == orig)
				ins->inst_target_bb = repl;
			break;
		case OP_SWITCH: {
			int i;
			int n = GPOINTER_TO_INT (ins->klass);
			for (i = 0; i < n; i++ ) {
				if (ins->inst_many_bb [i] == orig)
					ins->inst_many_bb [i] = repl;
			}
			break;
		}
		default:
			if (MONO_IS_COND_BRANCH_OP (ins)) {
				if (ins->inst_true_bb == orig)
					ins->inst_true_bb = repl;
				if (ins->inst_false_bb == orig)
					ins->inst_false_bb = repl;
			} else if (MONO_IS_JUMP_TABLE (ins)) {
				int i;
				MonoJumpInfoBBTable *table = MONO_JUMP_TABLE_FROM_INS (ins);
				for (i = 0; i < table->table_size; i++ ) {
					if (table->table [i] == orig)
						table->table [i] = repl;
				}
			}

			break;
		}
	}
}

/**
  * Check if a bb is useless (is just made of NOPs and ends with an
  * unconditional branch, or nothing).
  * If it is so, unlink it from the CFG and nullify it, and return TRUE.
  * Otherwise, return FALSE;
  */
static gboolean
remove_block_if_useless (MonoCompile *cfg, MonoBasicBlock *bb, MonoBasicBlock *previous_bb) {
	MonoBasicBlock *target_bb = NULL;
	MonoInst *inst;

	/* Do not touch handlers */
	if (bb->region != -1) {
		bb->not_useless = TRUE;
		return FALSE;
	}
	
	MONO_BB_FOR_EACH_INS (bb, inst) {
		switch (inst->opcode) {
		case OP_NOP:
			break;
		case OP_BR:
			target_bb = inst->inst_target_bb;
			break;
		default:
			bb->not_useless = TRUE;
			return FALSE;
		}
	}
	
	if (target_bb == NULL) {
		if ((bb->out_count == 1) && (bb->out_bb [0] == bb->next_bb)) {
			target_bb = bb->next_bb;
		} else {
			/* Do not touch empty BBs that do not "fall through" to their next BB (like the exit BB) */
			return FALSE;
		}
	}
	
	/* Do not touch BBs following a switch (they are the "default" branch) */
	if ((previous_bb->last_ins != NULL) && (previous_bb->last_ins->opcode == OP_SWITCH)) {
		return FALSE;
	}
	
	/* Do not touch BBs following the entry BB and jumping to something that is not */
	/* thiry "next" bb (the entry BB cannot contain the branch) */
	if ((previous_bb == cfg->bb_entry) && (bb->next_bb != target_bb)) {
		return FALSE;
	}

	/* 
	 * Do not touch BBs following a try block as the code in 
	 * mini_method_compile needs them to compute the length of the try block.
	 */
	if (MONO_BBLOCK_IS_IN_REGION (previous_bb, MONO_REGION_TRY))
		return FALSE;
	
	/* Check that there is a target BB, and that bb is not an empty loop (Bug 75061) */
	if ((target_bb != NULL) && (target_bb != bb)) {
		int i;

		if (cfg->verbose_level > 1) {
			printf ("remove_block_if_useless, removed BB%d\n", bb->block_num);
		}
		
		/* unlink_bblock () modifies the bb->in_bb array so can't use a for loop here */
		while (bb->in_count) {
			MonoBasicBlock *in_bb = bb->in_bb [0];
			mono_unlink_bblock (cfg, in_bb, bb);
			mono_link_bblock (cfg, in_bb, target_bb);
			replace_out_block_in_code (in_bb, bb, target_bb);
		}
		
		mono_unlink_bblock (cfg, bb, target_bb);
		
		if ((previous_bb != cfg->bb_entry) &&
				(previous_bb->region == bb->region) &&
				((previous_bb->last_ins == NULL) ||
				((previous_bb->last_ins->opcode != OP_BR) &&
				(! (MONO_IS_COND_BRANCH_OP (previous_bb->last_ins))) &&
				(previous_bb->last_ins->opcode != OP_SWITCH)))) {
			for (i = 0; i < previous_bb->out_count; i++) {
				if (previous_bb->out_bb [i] == target_bb) {
					MonoInst *jump;
					MONO_INST_NEW (cfg, jump, OP_BR);
					MONO_ADD_INS (previous_bb, jump);
					jump->cil_code = previous_bb->cil_code;
					jump->inst_target_bb = target_bb;
					break;
				}
			}
		}
		
		previous_bb->next_bb = bb->next_bb;
		mono_nullify_basic_block (bb);
		
		return TRUE;
	} else {
		return FALSE;
	}
}

void
mono_merge_basic_blocks (MonoCompile *cfg, MonoBasicBlock *bb, MonoBasicBlock *bbn) 
{
	MonoInst *inst;
	MonoBasicBlock *prev_bb;
	int i;

	bb->has_array_access |= bbn->has_array_access;
	bb->extended |= bbn->extended;

	mono_unlink_bblock (cfg, bb, bbn);
	for (i = 0; i < bbn->out_count; ++i)
		mono_link_bblock (cfg, bb, bbn->out_bb [i]);
	while (bbn->out_count)
		mono_unlink_bblock (cfg, bbn, bbn->out_bb [0]);

	/* Handle the branch at the end of the bb */
	for (inst = bb->code; inst != NULL; inst = inst->next) {
		if (inst->opcode == OP_CALL_HANDLER) {
			g_assert (inst->inst_target_bb == bbn);
			NULLIFY_INS (inst);
		}
		if (MONO_IS_JUMP_TABLE (inst)) {
			int i;
			MonoJumpInfoBBTable *table = MONO_JUMP_TABLE_FROM_INS (inst);
			for (i = 0; i < table->table_size; i++ ) {
				/* Might be already NULL from a previous merge */
				if (table->table [i])
					g_assert (table->table [i] == bbn);
				table->table [i] = NULL;
			}
			/* Can't nullify this as later instructions depend on it */
		}
	}
	if (bb->last_ins && MONO_IS_COND_BRANCH_OP (bb->last_ins)) {
		g_assert (bb->last_ins->inst_false_bb == bbn);
		bb->last_ins->inst_false_bb = NULL;
		bb->extended = TRUE;
	} else if (bb->last_ins && MONO_IS_BRANCH_OP (bb->last_ins)) {
		NULLIFY_INS (bb->last_ins);
	}

	if (bb->last_ins) {
		if (bbn->code) {
			bb->last_ins->next = bbn->code;
			bbn->code->prev = bb->last_ins;
			bb->last_ins = bbn->last_ins;
		}
	} else {
		bb->code = bbn->code;
		bb->last_ins = bbn->last_ins;
	}
	for (prev_bb = cfg->bb_entry; prev_bb && prev_bb->next_bb != bbn; prev_bb = prev_bb->next_bb)
		;
	if (prev_bb) {
		prev_bb->next_bb = bbn->next_bb;
	} else {
		/* bbn might not be in the bb list yet */
		if (bb->next_bb == bbn)
			bb->next_bb = bbn->next_bb;
	}
	mono_nullify_basic_block (bbn);
}

static void
move_basic_block_to_end (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoBasicBlock *bbn, *next;

	next = bb->next_bb;

	/* Find the previous */
	for (bbn = cfg->bb_entry; bbn->next_bb && bbn->next_bb != bb; bbn = bbn->next_bb)
		;
	if (bbn->next_bb) {
		bbn->next_bb = bb->next_bb;
	}

	/* Find the last */
	for (bbn = cfg->bb_entry; bbn->next_bb; bbn = bbn->next_bb)
		;
	bbn->next_bb = bb;
	bb->next_bb = NULL;

	/* Add a branch */
	if (next && (!bb->last_ins || ((bb->last_ins->opcode != OP_NOT_REACHED) && (bb->last_ins->opcode != OP_BR) && (bb->last_ins->opcode != OP_BR_REG) && (!MONO_IS_COND_BRANCH_OP (bb->last_ins))))) {
		MonoInst *ins;

		MONO_INST_NEW (cfg, ins, OP_BR);
		MONO_ADD_INS (bb, ins);
		mono_link_bblock (cfg, bb, next);
		ins->inst_target_bb = next;
	}		
}

/*
 * mono_remove_block:
 *
 *   Remove BB from the control flow graph
 */
void
mono_remove_bblock (MonoCompile *cfg, MonoBasicBlock *bb) 
{
	MonoBasicBlock *tmp_bb;

	for (tmp_bb = cfg->bb_entry; tmp_bb && tmp_bb->next_bb != bb; tmp_bb = tmp_bb->next_bb)
		;

	g_assert (tmp_bb);
	tmp_bb->next_bb = bb->next_bb;
}

void
mono_remove_critical_edges (MonoCompile *cfg)
{
	MonoBasicBlock *bb;
	MonoBasicBlock *previous_bb;
	
	if (cfg->verbose_level > 3) {
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
			int i;
			printf ("remove_critical_edges, BEFORE BB%d (in:", bb->block_num);
			for (i = 0; i < bb->in_count; i++) {
				printf (" %d", bb->in_bb [i]->block_num);
			}
			printf (") (out:");
			for (i = 0; i < bb->out_count; i++) {
				printf (" %d", bb->out_bb [i]->block_num);
			}
			printf (")");
			if (bb->last_ins != NULL) {
				printf (" ");
				mono_print_tree (bb->last_ins);
			}
			printf ("\n");
		}
	}
	
	for (previous_bb = cfg->bb_entry, bb = previous_bb->next_bb; bb != NULL; previous_bb = previous_bb->next_bb, bb = bb->next_bb) {
		if (bb->in_count > 1) {
			int in_bb_index;
			for (in_bb_index = 0; in_bb_index < bb->in_count; in_bb_index++) {
				MonoBasicBlock *in_bb = bb->in_bb [in_bb_index];
				if (in_bb->out_count > 1) {
					MonoBasicBlock *new_bb = mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoBasicBlock));
					new_bb->block_num = cfg->num_bblocks++;
//					new_bb->real_offset = bb->real_offset;
					new_bb->region = bb->region;
					
					/* Do not alter the CFG while altering the BB list */
					if (previous_bb->region == bb->region) {
						if (previous_bb != cfg->bb_entry) {
							/* If previous_bb "followed through" to bb, */
							/* keep it linked with a OP_BR */
							if ((previous_bb->last_ins == NULL) ||
									((previous_bb->last_ins->opcode != OP_BR) &&
									(! (MONO_IS_COND_BRANCH_OP (previous_bb->last_ins))) &&
									(previous_bb->last_ins->opcode != OP_SWITCH))) {
								int i;
								/* Make sure previous_bb really falls through bb */
								for (i = 0; i < previous_bb->out_count; i++) {
									if (previous_bb->out_bb [i] == bb) {
										MonoInst *jump;
										MONO_INST_NEW (cfg, jump, OP_BR);
										MONO_ADD_INS (previous_bb, jump);
										jump->cil_code = previous_bb->cil_code;
										jump->inst_target_bb = bb;
										break;
									}
								}
							}
						} else {
							/* We cannot add any inst to the entry BB, so we must */
							/* put a new BB in the middle to hold the OP_BR */
							MonoInst *jump;
							MonoBasicBlock *new_bb_after_entry = mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoBasicBlock));
							new_bb_after_entry->block_num = cfg->num_bblocks++;
//							new_bb_after_entry->real_offset = bb->real_offset;
							new_bb_after_entry->region = bb->region;
							
							MONO_INST_NEW (cfg, jump, OP_BR);
							MONO_ADD_INS (new_bb_after_entry, jump);
							jump->cil_code = bb->cil_code;
							jump->inst_target_bb = bb;
							
							previous_bb->next_bb = new_bb_after_entry;
							previous_bb = new_bb_after_entry;
							
							if (cfg->verbose_level > 2) {
								printf ("remove_critical_edges, added helper BB%d jumping to BB%d\n", new_bb_after_entry->block_num, bb->block_num);
							}
						}
					}
					
					/* Insert new_bb in the BB list */
					previous_bb->next_bb = new_bb;
					new_bb->next_bb = bb;
					previous_bb = new_bb;
					
					/* Setup in_bb and out_bb */
					new_bb->in_bb = mono_mempool_alloc ((cfg)->mempool, sizeof (MonoBasicBlock*));
					new_bb->in_bb [0] = in_bb;
					new_bb->in_count = 1;
					new_bb->out_bb = mono_mempool_alloc ((cfg)->mempool, sizeof (MonoBasicBlock*));
					new_bb->out_bb [0] = bb;
					new_bb->out_count = 1;
					
					/* Relink in_bb and bb to (from) new_bb */
					replace_out_block (in_bb, bb, new_bb);
					replace_out_block_in_code (in_bb, bb, new_bb);
					replace_in_block (bb, in_bb, new_bb);
					
					if (cfg->verbose_level > 2) {
						printf ("remove_critical_edges, removed critical edge from BB%d to BB%d (added BB%d)\n", in_bb->block_num, bb->block_num, new_bb->block_num);
					}
				}
			}
		}
	}
	
	if (cfg->verbose_level > 3) {
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
			int i;
			printf ("remove_critical_edges, AFTER BB%d (in:", bb->block_num);
			for (i = 0; i < bb->in_count; i++) {
				printf (" %d", bb->in_bb [i]->block_num);
			}
			printf (") (out:");
			for (i = 0; i < bb->out_count; i++) {
				printf (" %d", bb->out_bb [i]->block_num);
			}
			printf (")");
			if (bb->last_ins != NULL) {
				printf (" ");
				mono_print_tree (bb->last_ins);
			}
			printf ("\n");
		}
	}
}

/* checks that a and b represent the same instructions, conservatively,
 * it can return FALSE also for two trees that are equal.
 * FIXME: also make sure there are no side effects.
 */
static int
same_trees (MonoInst *a, MonoInst *b)
{
	int arity;
	if (a->opcode != b->opcode)
		return FALSE;
	arity = mono_burg_arity [a->opcode];
	if (arity == 1) {
		if (a->ssa_op == b->ssa_op && a->ssa_op == MONO_SSA_LOAD && a->inst_i0 == b->inst_i0)
			return TRUE;
		return same_trees (a->inst_left, b->inst_left);
	} else if (arity == 2) {
		return same_trees (a->inst_left, b->inst_left) && same_trees (a->inst_right, b->inst_right);
	} else if (arity == 0) {
		switch (a->opcode) {
		case OP_ICONST:
			return a->inst_c0 == b->inst_c0;
		default:
			return FALSE;
		}
	}
	return FALSE;
}

static int
get_unsigned_condbranch (int opcode)
{
	switch (opcode) {
	case CEE_BLE: return CEE_BLE_UN;
	case CEE_BLT: return CEE_BLT_UN;
	case CEE_BGE: return CEE_BGE_UN;
	case CEE_BGT: return CEE_BGT_UN;
	}
	g_assert_not_reached ();
	return 0;
}

static int
tree_is_unsigned (MonoInst* ins) {
	switch (ins->opcode) {
	case OP_ICONST:
		return (int)ins->inst_c0 >= 0;
	/* array lengths are positive as are string sizes */
	case CEE_LDLEN:
	case OP_STRLEN:
		return TRUE;
	case CEE_CONV_U1:
	case CEE_CONV_U2:
	case CEE_CONV_U4:
	case CEE_CONV_OVF_U1:
	case CEE_CONV_OVF_U2:
	case CEE_CONV_OVF_U4:
		return TRUE;
	case CEE_LDIND_U1:
	case CEE_LDIND_U2:
	case CEE_LDIND_U4:
		return TRUE;
	default:
		return FALSE;
	}
}

/* check if an unsigned compare can be used instead of two signed compares
 * for (val < 0 || val > limit) conditionals.
 * Returns TRUE if the optimization has been applied.
 * Note that this can't be applied if the second arg is not positive...
 */
static int
try_unsigned_compare (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoBasicBlock *truet, *falset;
	MonoInst *cmp_inst = bb->last_ins->inst_left;
	MonoInst *condb;
	if (!cmp_inst->inst_right->inst_c0 == 0)
		return FALSE;
	truet = bb->last_ins->inst_true_bb;
	falset = bb->last_ins->inst_false_bb;
	if (falset->in_count != 1)
		return FALSE;
	condb = falset->last_ins;
	/* target bb must have one instruction */
	if (!condb || (condb != falset->code))
		return FALSE;
	if ((((condb->opcode == CEE_BLE || condb->opcode == CEE_BLT) && (condb->inst_false_bb == truet))
			|| ((condb->opcode == CEE_BGE || condb->opcode == CEE_BGT) && (condb->inst_true_bb == truet)))
			&& same_trees (cmp_inst->inst_left, condb->inst_left->inst_left)) {
		if (!tree_is_unsigned (condb->inst_left->inst_right))
			return FALSE;
		condb->opcode = get_unsigned_condbranch (condb->opcode);
		/* change the original condbranch to just point to the new unsigned check */
		bb->last_ins->opcode = OP_BR;
		bb->last_ins->inst_target_bb = falset;
		replace_out_block (bb, truet, NULL);
		replace_in_block (truet, bb, NULL);
		return TRUE;
	}
	return FALSE;
}

/*
 * Optimizes the branches on the Control Flow Graph
 *
 */
void
mono_optimize_branches (MonoCompile *cfg)
{
	int i, changed = FALSE;
	MonoBasicBlock *bb, *bbn;
	guint32 niterations;

	/*
	 * Some crazy loops could cause the code below to go into an infinite
	 * loop, see bug #53003 for an example. To prevent this, we put an upper
	 * bound on the number of iterations.
	 */
	if (cfg->num_bblocks > 1000)
		niterations = cfg->num_bblocks * 2;
	else
		niterations = 1000;
	
	do {
		MonoBasicBlock *previous_bb;
		changed = FALSE;
		niterations --;

		/* we skip the entry block (exit is handled specially instead ) */
		for (previous_bb = cfg->bb_entry, bb = cfg->bb_entry->next_bb; bb; previous_bb = bb, bb = bb->next_bb) {
			/* dont touch code inside exception clauses */
			if (bb->region != -1)
				continue;

			if (!bb->not_useless && remove_block_if_useless (cfg, bb, previous_bb)) {
				changed = TRUE;
				continue;
			}

			if ((bbn = bb->next_bb) && bbn->in_count == 0 && bbn != cfg->bb_exit && bb->region == bbn->region) {
				if (cfg->verbose_level > 2)
					g_print ("nullify block triggered %d\n", bbn->block_num);

				bb->next_bb = bbn->next_bb;

				for (i = 0; i < bbn->out_count; i++)
					replace_in_block (bbn->out_bb [i], bbn, NULL);

				mono_nullify_basic_block (bbn);			
				changed = TRUE;
			}

			if (bb->out_count == 1) {
				bbn = bb->out_bb [0];

				/* conditional branches where true and false targets are the same can be also replaced with OP_BR */
				if (bb->last_ins && (bb->last_ins->opcode != OP_BR) && MONO_IS_COND_BRANCH_OP (bb->last_ins)) {
					if (!cfg->new_ir) {
						MonoInst *pop;
						MONO_INST_NEW (cfg, pop, CEE_POP);
						pop->inst_left = bb->last_ins->inst_left->inst_left;
						mono_add_ins_to_end (bb, pop);
						MONO_INST_NEW (cfg, pop, CEE_POP);
						pop->inst_left = bb->last_ins->inst_left->inst_right;
						mono_add_ins_to_end (bb, pop);
					}
					bb->last_ins->opcode = OP_BR;
					bb->last_ins->inst_target_bb = bb->last_ins->inst_true_bb;
					changed = TRUE;
					if (cfg->verbose_level > 2)
						g_print ("cond branch removal triggered in %d %d\n", bb->block_num, bb->out_count);
				}

				if (bb->region == bbn->region && bb->next_bb == bbn) {
					/* the block are in sequence anyway ... */

					/* branches to the following block can be removed */
					if (bb->last_ins && bb->last_ins->opcode == OP_BR) {
						bb->last_ins->opcode = OP_NOP;
						changed = TRUE;
						if (cfg->verbose_level > 2)
							g_print ("br removal triggered %d -> %d\n", bb->block_num, bbn->block_num);
					}

					if (bbn->in_count == 1 && !bb->extended) {
						if (bbn != cfg->bb_exit) {
							if (cfg->verbose_level > 2)
								g_print ("block merge triggered %d -> %d\n", bb->block_num, bbn->block_num);
							mono_merge_basic_blocks (cfg, bb, bbn);
							changed = TRUE;
							continue;
						}

						//mono_print_bb_code (bb);
					}
				}
			}

			if ((bbn = bb->next_bb) && bbn->in_count == 0 && bbn != cfg->bb_exit && bb->region == bbn->region) {
				if (cfg->verbose_level > 2) {
					g_print ("nullify block triggered %d\n", bbn->block_num);
				}
				bb->next_bb = bbn->next_bb;

				for (i = 0; i < bbn->out_count; i++)
					replace_in_block (bbn->out_bb [i], bbn, NULL);

				mono_nullify_basic_block (bbn);			
				changed = TRUE;
				continue;
			}

			if (bb->out_count == 1) {
				bbn = bb->out_bb [0];

				if (bb->last_ins && bb->last_ins->opcode == OP_BR) {
					bbn = bb->last_ins->inst_target_bb;
					if (bb->region == bbn->region && bbn->code && bbn->code->opcode == OP_BR &&
						bbn->code->inst_target_bb != bbn &&
					    bbn->code->inst_target_bb->region == bb->region) {
						
						if (cfg->verbose_level > 2)
							g_print ("branch to branch triggered %d -> %d -> %d\n", bb->block_num, bbn->block_num, bbn->code->inst_target_bb->block_num);

						replace_in_block (bbn, bb, NULL);
						replace_out_block (bb, bbn, bbn->code->inst_target_bb);
						mono_link_bblock (cfg, bb, bbn->code->inst_target_bb);
						bb->last_ins->inst_target_bb = bbn->code->inst_target_bb;
						changed = TRUE;
						continue;
					}
				}
			} else if (bb->out_count == 2) {
				if (bb->last_ins && MONO_IS_COND_BRANCH_NOFP (bb->last_ins)) {
					int branch_result;
					MonoBasicBlock *taken_branch_target = NULL, *untaken_branch_target = NULL;

					if (cfg->new_ir) {
						if (bb->last_ins->flags & MONO_INST_CFOLD_TAKEN)
							branch_result = BRANCH_TAKEN;
						else if (bb->last_ins->flags & MONO_INST_CFOLD_NOT_TAKEN)
							branch_result = BRANCH_NOT_TAKEN;
						else
							branch_result = BRANCH_UNDEF;
					}
					else
						branch_result = mono_eval_cond_branch (bb->last_ins);

					if (branch_result == BRANCH_TAKEN) {
						taken_branch_target = bb->last_ins->inst_true_bb;
						untaken_branch_target = bb->last_ins->inst_false_bb;
					} else if (branch_result == BRANCH_NOT_TAKEN) {
						taken_branch_target = bb->last_ins->inst_false_bb;
						untaken_branch_target = bb->last_ins->inst_true_bb;
					}
					if (taken_branch_target) {
						/* if mono_eval_cond_branch () is ever taken to handle 
						 * non-constant values to compare, issue a pop here.
						 */
						bb->last_ins->opcode = OP_BR;
						bb->last_ins->inst_target_bb = taken_branch_target;
						if (!bb->extended)
							mono_unlink_bblock (cfg, bb, untaken_branch_target);
						changed = TRUE;
						continue;
					}
					bbn = bb->last_ins->inst_true_bb;
					if (bb->region == bbn->region && bbn->code && bbn->code->opcode == OP_BR &&
					    bbn->code->inst_target_bb->region == bb->region) {
						if (cfg->verbose_level > 2)		
							g_print ("cbranch1 to branch triggered %d -> (%d) %d (0x%02x)\n", 
								 bb->block_num, bbn->block_num, bbn->code->inst_target_bb->block_num, 
								 bbn->code->opcode);

						/* 
						 * Unlink, then relink bblocks to avoid various
						 * tricky situations when the two targets of the branch
						 * are equal, or will become equal after the change.
						 */
						mono_unlink_bblock (cfg, bb, bb->last_ins->inst_true_bb);
						mono_unlink_bblock (cfg, bb, bb->last_ins->inst_false_bb);

						bb->last_ins->inst_true_bb = bbn->code->inst_target_bb;

						mono_link_bblock (cfg, bb, bb->last_ins->inst_true_bb);
						mono_link_bblock (cfg, bb, bb->last_ins->inst_false_bb);

						changed = TRUE;
						continue;
					}

					bbn = bb->last_ins->inst_false_bb;
					if (bbn && bb->region == bbn->region && bbn->code && bbn->code->opcode == OP_BR &&
					    bbn->code->inst_target_bb->region == bb->region) {
						if (cfg->verbose_level > 2)
							g_print ("cbranch2 to branch triggered %d -> (%d) %d (0x%02x)\n", 
								 bb->block_num, bbn->block_num, bbn->code->inst_target_bb->block_num, 
								 bbn->code->opcode);

						mono_unlink_bblock (cfg, bb, bb->last_ins->inst_true_bb);
						mono_unlink_bblock (cfg, bb, bb->last_ins->inst_false_bb);

						bb->last_ins->inst_false_bb = bbn->code->inst_target_bb;

						mono_link_bblock (cfg, bb, bb->last_ins->inst_true_bb);
						mono_link_bblock (cfg, bb, bb->last_ins->inst_false_bb);

						changed = TRUE;
						continue;
					}

					bbn = bb->last_ins->inst_false_bb;
					/*
					 * If bb is an extended bb, it could contain an inside branch to bbn.
					 * FIXME: Enable the optimization if that is not true.
					 * If bblocks_linked () is true, then merging bb and bbn
					 * would require addition of an extra branch at the end of bbn 
					 * slowing down loops.
					 */
					if (cfg->new_ir && bbn && bb->region == bbn->region && bbn->in_count == 1 && cfg->enable_extended_bblocks && bbn != cfg->bb_exit && !bb->extended && !bbn->out_of_line && !mono_bblocks_linked (bbn, bb)) {
						g_assert (bbn->in_bb [0] == bb);
						if (cfg->verbose_level > 2)
							g_print ("merge false branch target triggered BB%d -> BB%d\n", bb->block_num, bbn->block_num);
						mono_merge_basic_blocks (cfg, bb, bbn);
						changed = TRUE;
						continue;
					}
				}

				/* detect and optimize to unsigned compares checks like: if (v < 0 || v > limit */
				if (bb->last_ins && bb->last_ins->opcode == CEE_BLT && !cfg->new_ir && bb->last_ins->inst_left->inst_right->opcode == OP_ICONST) {
					if (try_unsigned_compare (cfg, bb)) {
						/*g_print ("applied in bb %d (->%d) %s\n", bb->block_num, bb->last_ins->inst_target_bb->block_num, mono_method_full_name (cfg->method, TRUE));*/
						changed = TRUE;
						continue;
					}
				}

				if (bb->last_ins && MONO_IS_COND_BRANCH_NOFP (bb->last_ins)) {
					if (bb->last_ins->inst_false_bb && bb->last_ins->inst_false_bb->out_of_line && (bb->region == bb->last_ins->inst_false_bb->region)) {
						/* Reverse the branch */
						bb->last_ins->opcode = mono_reverse_branch_op (bb->last_ins->opcode);
						bbn = bb->last_ins->inst_false_bb;
						bb->last_ins->inst_false_bb = bb->last_ins->inst_true_bb;
						bb->last_ins->inst_true_bb = bbn;

						move_basic_block_to_end (cfg, bb->last_ins->inst_true_bb);
						if (cfg->verbose_level > 2)
							g_print ("cbranch to throw block triggered %d.\n", 
									 bb->block_num);
					}
				}
			}
		}
	} while (changed && (niterations > 0));
}
