/*
    Copyright (C) 2019 Daniel Schultz

    This file is part of FLINT.

    FLINT is free software: you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.  See <https://www.gnu.org/licenses/>.
*/

#include "fmpz.h"
#include "fmpz_mod.h"

void fmpz_mod_ctx_clear(fmpz_mod_ctx_t ctx)
{
    if (ctx->ninv_huge != NULL)
    {
        fmpz_preinvn_clear(ctx->ninv_huge);
        flint_free(ctx->ninv_huge);
    }

    fmpz_clear(ctx->n);
}
