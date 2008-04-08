#ifndef __MONO_METADATA_VERIFY_INTERNAL_H__
#define __MONO_METADATA_VERIFY_INTERNAL_H__

#include <mono/metadata/metadata.h>

G_BEGIN_DECLS

typedef enum {
	MONO_VERIFIER_MODE_OFF,
	MONO_VERIFIER_MODE_VALID,
	MONO_VERIFIER_MODE_VERIFIABLE,
	MONO_VERIFIER_MODE_STRICT
} MiniVerifierMode;

void mono_verifier_set_mode (MiniVerifierMode mode) MONO_INTERNAL;
void mono_verifier_enable_verify_all (void) MONO_INTERNAL;

gboolean mono_verifier_is_enabled_for_method (MonoMethod *method) MONO_INTERNAL;
gboolean mono_verifier_is_enabled_for_class (MonoClass *klass) MONO_INTERNAL;

gboolean mono_verifier_is_method_full_trust (MonoMethod *method) MONO_INTERNAL;
gboolean mono_verifier_is_class_full_trust (MonoClass *klass) MONO_INTERNAL;

GSList* mono_method_verify_with_current_settings (MonoMethod *method, gboolean skip_visibility) MONO_INTERNAL;
G_END_DECLS

#endif  /* __MONO_METADATA_VERIFY_INTERNAL_H__ */

