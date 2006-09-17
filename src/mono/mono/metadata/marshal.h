
/*
 * marshal.h: Routines for marshaling complex types in P/Invoke methods.
 * 
 * Author:
 *   Paolo Molaro (lupus@ximian.com)
 *
 * (C) 2002 Ximian, Inc.  http://www.ximian.com
 *
 */

#ifndef __MONO_MARSHAL_H__
#define __MONO_MARSHAL_H__

#include <mono/metadata/class.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/opcodes.h>
#include <mono/metadata/reflection.h>

G_BEGIN_DECLS

typedef struct _MonoMethodBuilder MonoMethodBuilder;

/* marshaling helper functions */

void
mono_marshal_init (void);

void
mono_marshal_cleanup (void);

gint32
mono_class_native_size (MonoClass *klass, guint32 *align);

MonoMarshalType *
mono_marshal_load_type_info (MonoClass* klass);

gint32
mono_marshal_type_size (MonoType *type, MonoMarshalSpec *mspec, guint32 *align,
			gboolean as_field, gboolean unicode);

int            
mono_type_native_stack_size (MonoType *type, guint32 *alignment);

gpointer
mono_array_to_savearray (MonoArray *array);

gpointer
mono_array_to_lparray (MonoArray *array);

void
mono_string_utf8_to_builder (MonoStringBuilder *sb, char *text);

void
mono_string_utf16_to_builder (MonoStringBuilder *sb, gunichar2 *text);

gpointer
mono_string_builder_to_utf8 (MonoStringBuilder *sb);

gpointer
mono_string_builder_to_utf16 (MonoStringBuilder *sb);

gpointer
mono_string_to_ansibstr (MonoString *string_obj);

gpointer
mono_string_to_bstr (MonoString *string_obj);

void
mono_string_to_byvalstr (gpointer dst, MonoString *src, int size);

void
mono_string_to_byvalwstr (gpointer dst, MonoString *src, int size);

gpointer
mono_delegate_to_ftnptr (MonoDelegate *delegate);

MonoDelegate*
mono_ftnptr_to_delegate (MonoClass *klass, gpointer ftn);

void mono_delegate_free_ftnptr (MonoDelegate *delegate);

void
mono_marshal_set_last_error (void);

gpointer
mono_marshal_asany (MonoObject *obj, MonoMarshalNative string_encoding, int param_attrs);

void
mono_marshal_free_asany (MonoObject *o, gpointer ptr, MonoMarshalNative string_encoding, int param_attrs);

MonoMethod*
mono_marshal_get_write_barrier (void);

/* method builder functions */

void
mono_mb_free (MonoMethodBuilder *mb);

MonoMethodBuilder *
mono_mb_new (MonoClass *klass, const char *name, MonoWrapperType type);

void
mono_mb_patch_addr (MonoMethodBuilder *mb, int pos, int value);

void
mono_mb_patch_addr_s (MonoMethodBuilder *mb, int pos, gint8 value);

guint32
mono_mb_add_data (MonoMethodBuilder *mb, gpointer data);

void
mono_mb_emit_native_call (MonoMethodBuilder *mb, MonoMethodSignature *sig, gpointer func);

void
mono_mb_emit_managed_call (MonoMethodBuilder *mb, MonoMethod *method, MonoMethodSignature *opt_sig);

int
mono_mb_add_local (MonoMethodBuilder *mb, MonoType *type);

MonoMethod *
mono_mb_create_method (MonoMethodBuilder *mb, MonoMethodSignature *signature, int max_stack);

void
mono_mb_emit_ldarg (MonoMethodBuilder *mb, guint argnum);

void
mono_mb_emit_ldarg_addr (MonoMethodBuilder *mb, guint argnum);

void
mono_mb_emit_ldloc (MonoMethodBuilder *mb, guint num);

void
mono_mb_emit_ldloc_addr (MonoMethodBuilder *mb, guint locnum);

void
mono_mb_emit_stloc (MonoMethodBuilder *mb, guint num);

void
mono_mb_emit_exception (MonoMethodBuilder *mb, const char *exc_name, const char *msg);

void
mono_mb_emit_icon (MonoMethodBuilder *mb, gint32 value);

guint32
mono_mb_emit_branch (MonoMethodBuilder *mb, guint8 op);

guint32
mono_mb_emit_short_branch (MonoMethodBuilder *mb, guint8 op);

void
mono_mb_emit_add_to_local (MonoMethodBuilder *mb, guint16 local, gint32 incr);

void
mono_mb_emit_ldflda (MonoMethodBuilder *mb, gint32 offset);

void
mono_mb_emit_byte (MonoMethodBuilder *mb, guint8 op);

void
mono_mb_emit_i2 (MonoMethodBuilder *mb, gint16 data);

void
mono_mb_emit_i4 (MonoMethodBuilder *mb, gint32 data);

void
mono_mb_emit_ldstr (MonoMethodBuilder *mb, char *str);

guint
mono_type_to_ldind (MonoType *type);

guint
mono_type_to_stind (MonoType *type);

/* functions to create various architecture independent helper functions */

MonoMethod *
mono_marshal_method_from_wrapper (MonoMethod *wrapper);

MonoMethod *
mono_marshal_get_remoting_invoke (MonoMethod *method);

MonoMethod *
mono_marshal_get_xappdomain_invoke (MonoMethod *method);

MonoMethod *
mono_marshal_get_remoting_invoke_for_target (MonoMethod *method, MonoRemotingTarget target_type);

MonoMethod *
mono_marshal_get_remoting_invoke_with_check (MonoMethod *method);

MonoMethod *
mono_marshal_get_delegate_begin_invoke (MonoMethod *method);

MonoMethod *
mono_marshal_get_delegate_end_invoke (MonoMethod *method);

MonoMethod *
mono_marshal_get_delegate_invoke (MonoMethod *method);

MonoMethod *
mono_marshal_get_runtime_invoke (MonoMethod *method);

MonoMethod *
mono_marshal_get_managed_wrapper (MonoMethod *method, MonoClass *delegate_klass, MonoObject *this);

MonoMethod *
mono_marshal_get_icall_wrapper (MonoMethodSignature *sig, const char *name, gconstpointer func);

MonoMethod *
mono_marshal_get_native_wrapper (MonoMethod *method);

MonoMethod *
mono_marshal_get_native_func_wrapper (MonoImage *image, MonoMethodSignature *sig, MonoMethodPInvoke *piinfo, MonoMarshalSpec **mspecs, gpointer func);

MonoMethod *
mono_marshal_get_struct_to_ptr (MonoClass *klass);

MonoMethod *
mono_marshal_get_ptr_to_struct (MonoClass *klass);

MonoMethod *
mono_marshal_get_stfld_wrapper (MonoType *type);

MonoMethod *
mono_marshal_get_ldfld_wrapper (MonoType *type);

MonoMethod *
mono_marshal_get_ldflda_wrapper (MonoType *type);

MonoMethod *
mono_marshal_get_ldfld_remote_wrapper (MonoClass *klass);

MonoMethod *
mono_marshal_get_stfld_remote_wrapper (MonoClass *klass);

MonoMethod *
mono_marshal_get_synchronized_wrapper (MonoMethod *method);

MonoMethod *
mono_marshal_get_unbox_wrapper (MonoMethod *method);

MonoMethod *
mono_marshal_get_isinst (MonoClass *klass);

MonoMethod *
mono_marshal_get_castclass (MonoClass *klass);

MonoMethod *
mono_marshal_get_proxy_cancast (MonoClass *klass);

MonoMethod *
mono_marshal_get_stelemref (void);

MonoMethod *
mono_marshal_get_generic_array_helper (MonoClass *class, MonoClass *iface,
				       gchar *name, MonoMethod *method);

/* marshaling internal calls */

void * 
mono_marshal_alloc (gulong size);

void 
mono_marshal_free (gpointer ptr);

void
mono_marshal_free_array (gpointer *ptr, int size);

void * 
mono_marshal_realloc (gpointer ptr, gpointer size);

void
ves_icall_System_Runtime_InteropServices_Marshal_copy_to_unmanaged (MonoArray *src, gint32 start_index,
								    gpointer dest, gint32 length);

void
ves_icall_System_Runtime_InteropServices_Marshal_copy_from_unmanaged (gpointer src, gint32 start_index,
								      MonoArray *dest, gint32 length);

gpointer
ves_icall_System_Runtime_InteropServices_Marshal_ReadIntPtr (gpointer ptr, gint32 offset);

unsigned char
ves_icall_System_Runtime_InteropServices_Marshal_ReadByte (gpointer ptr, gint32 offset);

gint16
ves_icall_System_Runtime_InteropServices_Marshal_ReadInt16 (gpointer ptr, gint32 offset);

gint32
ves_icall_System_Runtime_InteropServices_Marshal_ReadInt32 (gpointer ptr, gint32 offset);

gint64
ves_icall_System_Runtime_InteropServices_Marshal_ReadInt64 (gpointer ptr, gint32 offset);

void
ves_icall_System_Runtime_InteropServices_Marshal_WriteByte (gpointer ptr, gint32 offset, unsigned char val);

void
ves_icall_System_Runtime_InteropServices_Marshal_WriteIntPtr (gpointer ptr, gint32 offset, gpointer val);

void
ves_icall_System_Runtime_InteropServices_Marshal_WriteInt16 (gpointer ptr, gint32 offset, gint16 val);

void
ves_icall_System_Runtime_InteropServices_Marshal_WriteInt32 (gpointer ptr, gint32 offset, gint32 val);

void
ves_icall_System_Runtime_InteropServices_Marshal_WriteInt64 (gpointer ptr, gint32 offset, gint64 val);

MonoString *
ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringAnsi (char *ptr);

MonoString *
ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringAnsi_len (char *ptr, gint32 len);

MonoString *
ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringUni (guint16 *ptr);

MonoString *
ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringUni_len (guint16 *ptr, gint32 len);

MonoString *
ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringBSTR (gpointer ptr);

guint32
ves_icall_System_Runtime_InteropServices_Marshal_GetComSlotForMethodInfoInternal (MonoReflectionMethod *m);

guint32 
ves_icall_System_Runtime_InteropServices_Marshal_GetLastWin32Error (void);

guint32 
ves_icall_System_Runtime_InteropServices_Marshal_SizeOf (MonoReflectionType *rtype);

void
ves_icall_System_Runtime_InteropServices_Marshal_StructureToPtr (MonoObject *obj, gpointer dst, MonoBoolean delete_old);

void
ves_icall_System_Runtime_InteropServices_Marshal_PtrToStructure (gpointer src, MonoObject *dst);

MonoObject *
ves_icall_System_Runtime_InteropServices_Marshal_PtrToStructure_type (gpointer src, MonoReflectionType *type);

int
ves_icall_System_Runtime_InteropServices_Marshal_OffsetOf (MonoReflectionType *type, MonoString *field_name);

gpointer
ves_icall_System_Runtime_InteropServices_Marshal_StringToBSTR (MonoString *string);

gpointer
ves_icall_System_Runtime_InteropServices_Marshal_StringToHGlobalAnsi (MonoString *string);

gpointer
ves_icall_System_Runtime_InteropServices_Marshal_StringToHGlobalUni (MonoString *string);

void
ves_icall_System_Runtime_InteropServices_Marshal_DestroyStructure (gpointer src, MonoReflectionType *type);

void*
ves_icall_System_Runtime_InteropServices_Marshal_AllocCoTaskMem (int size);

void
ves_icall_System_Runtime_InteropServices_Marshal_FreeCoTaskMem (void *ptr);

void*
ves_icall_System_Runtime_InteropServices_Marshal_AllocHGlobal (int size);

void
ves_icall_System_Runtime_InteropServices_Marshal_FreeHGlobal (void *ptr);

void
ves_icall_System_Runtime_InteropServices_Marshal_FreeBSTR (void *ptr);

void*
ves_icall_System_Runtime_InteropServices_Marshal_UnsafeAddrOfPinnedArrayElement (MonoArray *arrayobj, int index);

MonoDelegate*
ves_icall_System_Runtime_InteropServices_Marshal_GetDelegateForFunctionPointerInternal (void *ftn, MonoReflectionType *type);

int
ves_icall_System_Runtime_InteropServices_Marshal_AddRef (gpointer pUnk);

int
ves_icall_System_Runtime_InteropServices_Marshal_QueryInterface (gpointer pUnk, gpointer riid, gpointer* ppv);

int
ves_icall_System_Runtime_InteropServices_Marshal_Release (gpointer pUnk);


MonoObject *
ves_icall_System_ComObject_CreateRCW (MonoReflectionType *type);

void
ves_icall_System_ComObject_Finalizer(MonoComObject* obj);

gpointer
ves_icall_System_ComObject_FindInterface (MonoComObject* obj, MonoReflectionType* type);

void
ves_icall_System_ComObject_CacheInterface (MonoComObject* obj, MonoReflectionType* type, gpointer pItf);

gpointer
ves_icall_System_ComObject_GetIUnknown (MonoComObject* obj);

void
ves_icall_System_ComObject_SetIUnknown (MonoComObject* obj, gpointer pUnk);

G_END_DECLS

#endif /* __MONO_MARSHAL_H__ */

