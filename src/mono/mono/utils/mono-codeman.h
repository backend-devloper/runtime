#ifndef __MONO_CODEMAN_H__
#define __MONO_CODEMAN_H__

typedef struct _MonoCodeManager MonoCodeManager;

MonoCodeManager* mono_code_manager_new     (void);
void             mono_code_manager_destroy (MonoCodeManager *cman);
void             mono_code_manager_invalidate (MonoCodeManager *cman);

void*            mono_code_manager_reserve (MonoCodeManager *cman, int size);
void             mono_code_manager_commit  (MonoCodeManager *cman, void *data, int size, int newsize);


#endif /* __MONO_CODEMAN_H__ */

