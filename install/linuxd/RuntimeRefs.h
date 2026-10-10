#ifndef MATON_RUNTIME_REFS_H
#define MATON_RUNTIME_REFS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* All calls must hold the package operation mutex; live returns 1/0/-1 (unknown). */
typedef struct RuntimeRefs RuntimeRefs;
typedef int (*RuntimeRefsLive)(int, void*);
typedef int (*RuntimeRefsPrune)(const char*, int*, void*);
/* Missing file is a valid empty database; invalid files fail closed. */
RuntimeRefs* runtime_refs_open(const char* directory, unsigned user);
int runtime_refs_add(RuntimeRefs*, const char* ref, int uid);
/* Commit an exact set already protected by runtime_refs_add/save. */
int runtime_refs_replace(RuntimeRefs*, int uid, const char* const* refs, size_t count);
int runtime_refs_save(RuntimeRefs*);
int runtime_refs_remove(RuntimeRefs*, int uid);
int runtime_refs_reconcile(RuntimeRefs*, RuntimeRefsLive, void*);
int runtime_refs_prune(RuntimeRefs*, RuntimeRefsPrune, void*);
void runtime_refs_close(RuntimeRefs*);
#ifdef __cplusplus
}
#endif
#endif
