#ifndef THORN_MAKE_H
#define THORN_MAKE_H

#include "engine.h"

int emit_makefile(const Graph *g, const char *cc, const char *ar,
                  const char *path);

#endif /* THORN_MAKE_H */
