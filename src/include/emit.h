#ifndef THORN_EMIT_H
#define THORN_EMIT_H

#include "parser.h"

void apply_target_hooks(Target *t);
void map_ninja(const NDoc *doc, Graph *g, const char *pfx,
               char ***notes, size_t *nnotes);
void map_make(const MDoc *doc, Graph *g, const char *pfx,
              char ***notes, size_t *nnotes);

#endif /* THORN_EMIT_H */
