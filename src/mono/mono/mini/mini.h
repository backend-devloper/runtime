#ifndef __MONO_MINI_H__
#define __MONO_MINI_H__

#include <glib.h>
#include <signal.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/mempool.h>
#include <mono/utils/monobitset.h>
#include <mono/metadata/class.h>
#include <mono/metadata/object.h>
#include <mono/metadata/opcodes.h>
#include <mono/metadata/tabledefs.h>
#include "regalloc.h"

/* fixme: configure should set this */
#define SIZEOF_VOID_P 4

#define MONO_USE_AOT_COMPILER

#if 1
#define mono_bitset_test_fast(set,n) (((guint32*)set)[2+(n)/32] & (1 << ((n) % 32)))
#else
#define mono_bitset_test_fast(set,n) mono_bitset_test(set,n)
#endif

#if 0
#define mono_bitset_foreach_bit(set,b,n) \
	for (b = 0; b < n; b++)\
		if (mono_bitset_test_fast(set,b))
#define mono_bitset_foreach_bit_rev(set,b,n) \
	for (b = n - 1; b >= 0; b--)\
		if (mono_bitset_test_fast(set,b))
#else
#define mono_bitset_foreach_bit(set,b,n) \
	for (b = mono_bitset_find_first (set, -1); b < n && b >= 0; b = mono_bitset_find_first (set, b))
#define mono_bitset_foreach_bit_rev(set,b,n) \
	for (b = mono_bitset_find_last (set, n - 1); b >= 0; b = b ? mono_bitset_find_last (set, b) : -1)
 
#endif

/*
 * Pull the list of opcodes
 */
#define OPDEF(a,b,c,d,e,f,g,h,i,j) \
	a = i,

enum {
#include "mono/cil/opcode.def"
	CEE_LASTOP
};
#undef OPDEF

#define MONO_VARINFO(cfg,varnum) ((cfg)->vars [varnum])

#define MONO_INST_NEW(cfg,dest,op) do {	\
		(dest) = mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoInst));	\
		(dest)->opcode = (op);	\
	} while (0)

#define MONO_INST_NEW_CALL(cfg,dest,op) do {	\
		(dest) = mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoCallInst));	\
		(dest)->inst.opcode = (op);	\
	} while (0)

#define MONO_ADD_INS(b,inst) do {	\
		if ((b)->last_ins) {	\
			(b)->last_ins->next = (inst);	\
			(b)->last_ins = (inst);	\
		} else {	\
			(b)->code = (b)->last_ins = (inst);	\
		}	\
	} while (0)

typedef struct MonoInst MonoInst;
typedef struct MonoCallInst MonoCallInst;
typedef struct MonoEdge MonoEdge;
typedef struct MonoMethodVar MonoMethodVar;
typedef struct MonoBasicBlock MonoBasicBlock;
typedef struct MonoLMF MonoLMF;
typedef struct MonoSpillInfo MonoSpillInfo;

extern guint32 mono_jit_tls_id;
extern gboolean mono_jit_trace_calls;
extern gboolean mono_break_on_exc;
extern int mono_exc_esp_offset;
extern gboolean mono_compile_aot;
extern gboolean mono_trace_coverage;
extern gboolean mono_jit_profile;

extern CRITICAL_SECTION *metadata_section;

struct MonoEdge {
	MonoEdge *next;
	MonoBasicBlock *bb;
	/* add edge type? */
};

struct MonoSpillInfo {
	MonoSpillInfo *next;
	int offset;
};

/*
 * The IR-level basic block.  
 *
 * A basic block can have multiple exits just fine, as long as the point of
 * 'departure' is the last instruction in the basic block. Extended basic
 * blocks, on the other hand, may have instructions that leave the block
 * midstream. The important thing is that they cannot be _entered_
 * midstream, ie, execution of a basic block (or extened bb) always start
 * at the beginning of the block, never in the middle.
 */
struct MonoBasicBlock {
	MonoInst *last_ins;

	/* Points to the start of the CIL code that initiated this BB */
	unsigned char* cil_code;

	/* Length of the CIL block */
	gint32 cil_length;

	/* The address of the generated code, used for fixups */
	int native_offset;
	int max_offset;
	
	gint32 dfn;

	/* unique block number identification */
	gint32 block_num;

	/* Visited and reachable flags */
	guint32 flags;

	/* Basic blocks: incoming and outgoing counts and pointers */
	gint16 out_count, in_count;
	MonoBasicBlock **in_bb;
	MonoBasicBlock **out_bb;

	/* the next basic block in the order it appears in IL */
	MonoBasicBlock *next_bb;

	/*
	 * Before instruction selection it is the first tree in the
	 * forest and the first item in the list of trees. After
	 * instruction selection it is the first instruction and the
	 * first item in the list of instructions.
	 */
	MonoInst *code;

	/*
	 * SSA and loop based flags
	 */
	MonoBitSet *dominators;
	MonoBitSet *dfrontier;
	MonoBasicBlock *idom;
	GList *dominated;
	/* fast dominator algorithm */
	MonoBasicBlock *df_parent, *ancestor, *child, *label;
	MonoEdge *bucket;
	int size, sdom, idomn;
	
	/* loop nesting and recognition */
	GList *loop_blocks;
	gint8  nesting;

	/* use for liveness analysis */
	MonoBitSet *gen_set;
	MonoBitSet *kill_set;
	MonoBitSet *live_in_set;
	MonoBitSet *live_out_set;

	/* fields to deal with non-empty stack slots at bb boundary */
	guint16 out_scount, in_scount;
	MonoInst **out_stack;
	MonoInst **in_stack;

	/* we use that to prevent merging of bblock covered by different clauses*/
	guint real_offset;
        guint region;

	/* The current symbolic register number, used in local register allocation. */
	guint16 max_ireg, max_freg;
};

/* BBlock flags */
#define BB_VISITED 1
#define BB_REACHABLE 2

struct MonoInst {
	union {
		union {
			MonoInst *src;
			MonoMethodVar *var;
			gint32 const_val;
			gpointer p;
			MonoMethod *method;
			MonoMethodSignature *signature;
			MonoBasicBlock **many_blocks;
			MonoBasicBlock *target_block;
			MonoInst **args;
			MonoType *vtype;
			MonoClass *klass;
			int *phi_args;
		} op [2];
		gint64 i8const;
		double r8const;
	} data;
	guint16 opcode;
	guint8  type; /* stack type */
	guint   ssa_op : 3;
	guint8  flags  : 5;
	
	/* used by the register allocator */
	gint16 dreg, sreg1, sreg2, unused;
	
	MonoInst *next;
	MonoClass *klass;
	const unsigned char* cil_code; /* for debugging and bblock splitting */
};
	
struct MonoCallInst {
	MonoInst inst;
	MonoMethodSignature *signature;
	MonoMethod *method;
	MonoInst **args;
	gconstpointer fptr;
	guint stack_usage;
	guint32 used_iregs;
	guint32 used_fregs;
};

/* 
 * flags for MonoInst
 * Note: some of the values overlap, because they can't appear
 * in the same MonoInst.
 */
enum {
	MONO_INST_HAS_METHOD = 1,
	/* temp local created by a DUP: used only within a BB */
	MONO_INST_IS_TEMP    = 1,
	MONO_INST_INIT       = 1, /* in localloc */
	MONO_INST_IS_DEAD    = 2,
	MONO_INST_TAILCALL   = 4,
	MONO_INST_VOLATILE   = 4,
	MONO_INST_BRLABEL    = 4,
	MONO_INST_UNALIGNED  = 8,
	/* the address of the variable has been taken */
	MONO_INST_INDIRECT   = 16
};

#define inst_c0 data.op[0].const_val
#define inst_c1 data.op[1].const_val
#define inst_i0 data.op[0].src
#define inst_i1 data.op[1].src
#define inst_p0 data.op[0].p
#define inst_p1 data.op[1].p
#define inst_l  data.i8const
#define inst_r  data.r8const
#define inst_left  data.op[0].src
#define inst_right data.op[1].src

#define inst_newa_len   data.op[0].src
#define inst_newa_class data.op[1].klass

#define inst_switch data.op[0].switch_blocks
#define inst_var    data.op[0].var
#define inst_vtype  data.op[1].vtype
/* in branch instructions */
#define inst_many_bb   data.op[1].many_blocks
#define inst_target_bb data.op[0].target_block
#define inst_true_bb   data.op[1].many_blocks[0]
#define inst_false_bb  data.op[1].many_blocks[1]

#define inst_basereg sreg1
#define inst_indexreg sreg2
#define inst_destbasereg dreg
#define inst_offset data.op[0].const_val
#define inst_imm    data.op[1].const_val

#define inst_phi_args   data.op[1].phi_args

/* instruction description for use in regalloc/scheduling */
enum {
	MONO_INST_DEST,
	MONO_INST_SRC1,
	MONO_INST_SRC2,
	MONO_INST_FLAGS,
	MONO_INST_CLOB,
	MONO_INST_COST,
	MONO_INST_DELAY,
	MONO_INST_RES,
	MONO_INST_LEN,
	MONO_INST_MAX
};

typedef union {
	struct {
		guint16 tid; /* tree number */
		guint16 bid; /* block number */
	} pos ;
	guint32 abs_pos; 
} MonoPosition;

typedef struct {
	MonoPosition first_use, last_use;
} MonoLiveRange;

/*
 * Additional information about a variable
 */
struct MonoMethodVar {
	guint           idx; /* inside cfg->varinfo, cfg->vars */
	guint           last_name;
	MonoBitSet     *dfrontier;
	MonoLiveRange   range; /* generated by liveness analysis */
	int             reg; /* != -1 if allocated into a register */
	int             spill_costs;
	MonoBitSet     *def_in; /* used by SSA */
	MonoInst       *def;    /* used by SSA */
	MonoBasicBlock *def_bb; /* used by SSA */
	GList          *uses;   /* used by SSA */
	char            cpstate;  /* used by SSA conditional  constant propagation */
};

typedef struct {
	gpointer          end_of_stack;
	MonoLMF          *lmf;
	void            (*abort_func) (MonoObject *object);
} MonoJitTlsData;

typedef enum {
	MONO_PATCH_INFO_BB,
	MONO_PATCH_INFO_ABS,
	MONO_PATCH_INFO_LABEL,
	MONO_PATCH_INFO_METHOD,
	MONO_PATCH_INFO_METHOD_JUMP,
	MONO_PATCH_INFO_METHODCONST,
	MONO_PATCH_INFO_INTERNAL_METHOD,
	MONO_PATCH_INFO_SWITCH,
	MONO_PATCH_INFO_EXC,
	MONO_PATCH_INFO_CLASS,
        MONO_PATCH_INFO_IMAGE,
        MONO_PATCH_INFO_FIELD,
        MONO_PATCH_INFO_R4,
	MONO_PATCH_INFO_R8,
	MONO_PATCH_INFO_IP
} MonoJumpInfoType;

typedef struct MonoJumpInfo MonoJumpInfo;
struct MonoJumpInfo {
	MonoJumpInfo *next;
	union {
		int i;
		guint8 *p;
		MonoInst *label;
	} ip;

	MonoJumpInfoType type;
	union {
		gconstpointer   target;
		int             offset;
		MonoBasicBlock *bb;
		MonoBasicBlock **table;
		MonoInst       *inst;
		MonoMethod     *method;
		MonoClass      *klass;
		MonoClassField *field;
		MonoImage      *image;
		const char     *name;
	} data;

	int table_size; /* use by switch */
};

/* optimization flags: keep up to date with the name array in mini.c */
enum {
	MONO_OPT_PEEPHOLE = 1 << 0,
	MONO_OPT_BRANCH   = 1 << 1,
	MONO_OPT_INLINE   = 1 << 2,
	MONO_OPT_CFOLD    = 1 << 3,
	MONO_OPT_CONSPROP = 1 << 4,
	MONO_OPT_COPYPROP = 1 << 5,
	MONO_OPT_DEADCE   = 1 << 6,
	MONO_OPT_LINEARS  = 1 << 7,
	MONO_OPT_CMOV     = 1 << 8,
	MONO_OPT_SHARED   = 1 << 9,
	MONO_OPT_SCHED    = 1 << 10,
	MONO_OPT_INTRINS  = 1 << 11,
	MONO_OPT_TAILC    = 1 << 12,
	MONO_OPT_LOOP     = 1 << 13,
	MONO_OPT_FCMOV    = 1 << 14
};

/*
 * Control Flow Graph and compilation unit information
 */
typedef struct {
	MonoMethod      *method;
	MonoMemPool     *mempool;
	MonoInst       **varinfo;
	MonoMethodVar  **vars;
	MonoInst        *ret;
	MonoBasicBlock  *bb_entry;
	MonoBasicBlock  *bb_exit;
	MonoBasicBlock  *bb_init;
	MonoBasicBlock **bblocks;
	GHashTable      *bb_hash;
	MonoMemPool     *state_pool; /* used by instruction selection */
	MonoBasicBlock  *cbb;        /* used by instruction selection */
	MonoInst        *prev_ins;   /* in decompose */
	MonoJumpInfo    *patch_info;
	guint            num_bblocks;
	guint            locals_start;
	guint            num_varinfo; /* used items in varinfo */
	guint            varinfo_count; /* total storage in varinfo */
	gint             stack_offset;
	MonoRegState    *rs;
	MonoSpillInfo   *spill_info;
	gint             spill_count;
	// unsigned char   *cil_code;

	MonoInst        *exvar; /* the exception object passed to catch/filter blocks */
	MonoInst        *domainvar; /* a cache for the current domain */

	GList           *ldstr_list; /* used by AOT */
	
	MonoDomain      *domain;

	unsigned char   *native_code;
	guint            code_size;
	guint            code_len;
	guint            prolog_end;
	guint            epilog_begin;
	guint32          used_int_regs;
	guint32          opt;
	guint32          flags;
	guint32          comp_done;
	guint32          verbose_level;
	guint32          stack_usage;
	guint32          param_area;
	guint32          frame_reg;
	gboolean         disable_aot;
	gboolean         disable_ssa;
	gpointer         debug_info;
	guint16          *intvars;
} MonoCompile;

typedef enum {
	MONO_CFG_HAS_ALLOCA = 1 << 0,
	MONO_CFG_HAS_CALLS  = 1 << 1
} MonoCompileFlags;

typedef struct {
	int entries;
	struct {
		int iloffset;
		int count;
	} data [0];
} MonoCoverageInfo;

typedef struct {
	gulong methods_compiled;
	gulong methods_aot;
	gulong methods_lookups;
	gulong method_trampolines;
	gulong allocate_var;
	gulong analyze_stack_repeat;
	gulong cil_code_size;
	gulong native_code_size;
	gulong code_reallocs;
	gulong max_code_size_ratio;
	gulong biggest_method_size;
	gulong allocated_code_size;
	gulong inlineable_methods;
	gulong inlined_methods;
	gulong basic_blocks;
	gulong max_basic_blocks;
	MonoMethod *max_ratio_method;
	MonoMethod *biggest_method;
	gboolean enabled;
} MonoJitStats;

extern MonoJitStats mono_jit_stats;

/* values for MonoInst.ssa_op */
enum {
	MONO_SSA_NOP,
	MONO_SSA_LOAD,
	MONO_SSA_STORE,
	MONO_SSA_MAYBE_LOAD,
	MONO_SSA_MAYBE_STORE
};

#define OP_CEQ    (256+CEE_CEQ)
#define OP_CLT    (256+CEE_CLT)
#define OP_CLT_UN (256+CEE_CLT_UN)
#define OP_CGT    (256+CEE_CGT)
#define OP_CGT_UN (256+CEE_CGT_UN)
#define OP_LOCALLOC (256+CEE_LOCALLOC)

/* opcodes: value assigned after all the CIL opcodes */
#ifdef MINI_OP
#undef MINI_OP
#endif
#define MINI_OP(a,b) a,
enum {
	OP_START = MONO_CEE_LAST,
#include "mini-ops.h"
	OP_LAST
};
#undef MINI_OP

/* make this depend on 32bit platform (use OP_LADD otherwise) */
#define OP_PADD CEE_ADD
#define OP_PNEG CEE_NEG
#define OP_PCONV_TO_U2 CEE_CONV_U2
#define OP_PCONV_TO_OVF_I1_UN CEE_CONV_OVF_I1_UN
#define OP_PCONV_TO_OVF_I1 CEE_CONV_OVF_I1
#define OP_PCEQ CEE_CEQ

typedef enum {
	STACK_INV,
	STACK_I4,
	STACK_I8,
	STACK_PTR,
	STACK_R8,
	STACK_MP,
	STACK_OBJ,
	STACK_VTYPE,
	STACK_MAX
} MonoStackType;

typedef struct {
	union {
		double   r8;
		gint32   i4;
		gint64   i8;
		gpointer p;
		MonoClass *klass;
	} data;
	int type;
} StackSlot;

enum {
	MONO_COMP_DOM = 1,
	MONO_COMP_IDOM = 2,
	MONO_COMP_DFRONTIER = 4,
	MONO_COMP_DOM_REV = 8,
	MONO_COMP_LIVENESS = 16,
	MONO_COMP_SSA = 32,
	MONO_COMP_SSA_DEF_USE = 64,
	MONO_COMP_REACHABILITY = 128,
	MONO_COMP_LOOPS = 256
};

typedef enum {
	MONO_GRAPH_CFG = 1,
	MONO_GRAPH_DTREE = 2,
	MONO_GRAPH_CFG_CODE = 4,
	MONO_GRAPH_CFG_SSA = 8,
	MONO_GRAPH_CFG_OPTCODE = 16
} MonoGraphOptions;

typedef struct {
	char *name;
	gconstpointer func;
	gconstpointer wrapper;
	MonoMethodSignature *sig;
} MonoJitICallInfo;

typedef void (*MonoInstFunc) (MonoInst *tree, gpointer data);

/* main function */
int         mono_main                      (int argc, char* argv[]);
void        mono_set_defaults              (int verbose_level, guint32 opts);
MonoDomain* mini_init                      (const char *filename);
void        mini_cleanup                   (MonoDomain *domain);

MonoDomain* mono_jit_init                  (const char *filename);
void        mono_jit_cleanup               (MonoDomain *domain);

/* helper methods */
int       mono_parse_default_optimizations  (const char* p);
void      mono_bblock_add_inst              (MonoBasicBlock *bb, MonoInst *inst);
void      mono_constant_fold                (MonoCompile *cfg);
void      mono_constant_fold_inst           (MonoInst *inst, gpointer data);
void      mono_cprop_local                  (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst **acp, int acp_size);
MonoInst* mono_compile_create_var           (MonoCompile *cfg, MonoType *type, int opcode);
void      mono_blockset_print               (MonoCompile *cfg, MonoBitSet *set, const char *name, guint idom);
void      mono_print_tree                   (MonoInst *tree);
int       mono_spillvar_offset              (MonoCompile *cfg, int spillvar);
void      mono_select_instructions          (MonoCompile *cfg);
const char* mono_inst_name                  (int op);
void      mono_inst_foreach                 (MonoInst *tree, MonoInstFunc func, gpointer data);
void      mono_disassemble_code             (guint8 *code, int size, char *id);
guint     mono_type_to_ldind                (MonoType *t);
guint     mono_type_to_stind                (MonoType *t);
void      mono_add_patch_info               (MonoCompile *cfg, int ip, MonoJumpInfoType type, gconstpointer target);
void      mono_remove_patch_info            (MonoCompile *cfg, int ip);
gpointer  mono_get_lmf_addr                 (void);
GList    *mono_varlist_insert_sorted        (MonoCompile *cfg, GList *list, MonoMethodVar *mv, gboolean sort_end);
void      mono_analyze_liveness             (MonoCompile *cfg);
void      mono_linear_scan                  (MonoCompile *cfg, GList *vars, GList *regs, guint32 *used_mask);
void      mono_create_jump_table            (MonoCompile *cfg, MonoInst *label, MonoBasicBlock **bbs, int num_blocks);
int       mono_compile_assembly             (MonoAssembly *ass, guint32 opts);
MonoCompile *mini_method_compile            (MonoMethod *method, guint32 opts, MonoDomain *domain, int parts);
void      mono_destroy_compile              (MonoCompile *cfg);
gpointer  mono_aot_get_method               (MonoMethod *method);
gboolean  mono_method_blittable             (MonoMethod *method);
void      mono_register_opcode_emulation    (int opcode, MonoMethodSignature *sig, gpointer func);
void      mono_arch_register_lowlevel_calls (void);
void      mono_draw_graph                   (MonoCompile *cfg, MonoGraphOptions draw_options);
void      mono_add_varcopy_to_end           (MonoCompile *cfg, MonoBasicBlock *bb, int src, int dest);

int               mono_find_method_opcode      (MonoMethod *method);
MonoJitICallInfo *mono_find_jit_icall_by_name  (const char *name);
MonoJitICallInfo *mono_find_jit_icall_by_addr  (gconstpointer addr);
MonoJitICallInfo *mono_register_jit_icall      (gconstpointer func, const char *name, MonoMethodSignature *sig, gboolean is_save);

MonoCoverageInfo *mono_allocate_coverage_info (MonoMethod *method, int size);
MonoCoverageInfo *mono_get_coverage_info      (MonoMethod *method);

/* methods that must be provided by the arch-specific port */
guint32   mono_arch_cpu_optimizazions           (guint32 *exclude_mask);
void      mono_arch_instrument_mem_needs        (MonoMethod *method, int *stack, int *code);
void     *mono_arch_instrument_prolog           (MonoCompile *cfg, void *func, void *p, gboolean enable_arguments);
void     *mono_arch_instrument_epilog           (MonoCompile *cfg, void *func, void *p, gboolean enable_arguments);
MonoCallInst *mono_arch_call_opcode             (MonoCompile *cfg, MonoBasicBlock* bb, MonoCallInst *call, int is_virtual);
void      mono_codegen                          (MonoCompile *cfg);
const char *mono_arch_regname                   (int reg);
gpointer  mono_arch_get_throw_exception         (void);
gpointer  mono_arch_get_throw_exception_by_name (void);
gpointer  mono_arch_create_jit_trampoline       (MonoMethod *method);
gpointer  mono_arch_create_jump_trampoline      (MonoMethod *method);
GList    *mono_arch_get_allocatable_int_vars    (MonoCompile *cfg);
GList    *mono_arch_get_global_int_regs         (MonoCompile *cfg);
void      mono_arch_patch_code                  (MonoMethod *method, MonoDomain *domain, guint8 *code, MonoJumpInfo *ji);
void      mono_arch_flush_icache                (guint8 *code, gint size);
int       mono_arch_max_epilog_size             (MonoCompile *cfg);
guint8   *mono_arch_emit_prolog                 (MonoCompile *cfg);
void      mono_arch_emit_epilog                 (MonoCompile *cfg);
void      mono_arch_local_regalloc              (MonoCompile *cfg, MonoBasicBlock *bb);
void      mono_arch_output_basic_block          (MonoCompile *cfg, MonoBasicBlock *bb);
gboolean  mono_arch_has_unwind_info             (gconstpointer addr);
void      mono_arch_allocate_vars               (MonoCompile *m);
void      mono_jit_walk_stack                   (MonoStackWalk func, gpointer user_data);
MonoArray *ves_icall_get_trace                  (MonoException *exc, gint32 skip, MonoBoolean need_file_info);
MonoBoolean ves_icall_get_frame_info            (gint32 skip, MonoBoolean need_file_info, 
						 MonoReflectionMethod **method, 
						 gint32 *iloffset, gint32 *native_offset,
						 MonoString **file, gint32 *line, gint32 *column);

/* Dominator/SSA methods */
void        mono_compile_dominator_info         (MonoCompile *cfg, int dom_flags);
void        mono_compute_natural_loops          (MonoCompile *cfg);
MonoBitSet* mono_compile_iterated_dfrontier     (MonoCompile *cfg, MonoBitSet *set);
void        mono_ssa_compute                    (MonoCompile *cfg);
void        mono_ssa_remove                     (MonoCompile *cfg);
void        mono_ssa_cprop                      (MonoCompile *cfg);
void        mono_ssa_deadce                     (MonoCompile *cfg);
void        mono_ssa_strength_reduction         (MonoCompile *cfg);

/* debugging support */
void      mono_debug_init_method                (MonoCompile *cfg, MonoBasicBlock *start_block,
						 guint32 breakpoint_id);
void      mono_debug_open_method                (MonoCompile *cfg);
void      mono_debug_close_method               (MonoCompile *cfg);
void      mono_debug_open_block                 (MonoCompile *cfg, MonoBasicBlock *bb, guint32 address);
void      mono_debug_record_line_number         (MonoCompile *cfg, MonoInst *ins, guint32 address);

#endif /* __MONO_MINI_H__ */  
