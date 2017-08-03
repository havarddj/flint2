/*
    Copyright (C) 2017 Luca De Feo

    This file is part of FLINT.

    FLINT is free software: you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.  See <http://www.gnu.org/licenses/>.
*/

#include "templates.h"
#include "fq_poly.h"

#include <stdio.h>
#include <stdlib.h>

#include "ulong_extras.h"
#include "long_extras.h"

int
main(void)
{
    int i, result;
    FLINT_TEST_INIT(state);

    flint_printf("set_mod_poly... ");
    fflush(stdout);

    /* Check litfed polynomials by evaluating at random points */
    for (i = 0; i < 100 * flint_test_multiplier(); i++)
    {
        slong len;
        fq_ctx_t ctx;
        fq_poly_t a;
	fmpz_mod_poly_t b;
	fmpz_t p;
	fq_t r, s;

        len = n_randint(state, 15) + 1;
        fq_ctx_randtest(ctx, state);
        fq_poly_init(a, ctx);
        fmpz_mod_poly_init(b, &ctx->p);
	fmpz_init(p);
	fq_init(r, ctx); fq_init(s, ctx);

        fmpz_mod_poly_randtest(b, state, len);
	fmpz_randtest(p, state, 10);
	
        fq_poly_set_mod_poly(a, b, ctx);
	fq_set_fmpz(r, p, ctx);
	fq_poly_evaluate_fq(r, a, r, ctx);
	fmpz_mod_poly_evaluate_fmpz(p, b, p);
	fq_set_fmpz(s, p, ctx);

        result = fq_equal(r, s, ctx);
        if (!result)
        {
            flint_printf("FAIL:\n\n");
            flint_printf("CTX\n"), fq_ctx_print(ctx),
                flint_printf("\n");
            flint_printf("b = "), fmpz_mod_poly_print_pretty(b, "X"),
                flint_printf("\n");
            flint_printf("p = "), fmpz_print(p),
                flint_printf("\n");
            abort();
        }

        fq_clear(r, ctx); fq_clear(s, ctx);
	fmpz_clear(p);
	fmpz_mod_poly_clear(b);
	fq_poly_clear(a, ctx);
        fq_ctx_clear(ctx);
    }

    FLINT_TEST_CLEANUP(state);
    flint_printf("PASS\n");
    return EXIT_SUCCESS;
}