/**
 * @file formula_evaluator.c
 * @brief Formula evaluation for calculated sensors
 *
 * Uses TinyExpr library when available, otherwise provides simple fallback.
 */

#include "formula_evaluator.h"
#include "utils/logger.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifdef HAVE_TINYEXPR
#include <tinyexpr.h>
#define FORMULA_FULL_SUPPORT 1
#else
#define FORMULA_FULL_SUPPORT 0
#endif

#if FORMULA_FULL_SUPPORT

/* Full TinyExpr implementation */

result_t formula_evaluator_init(formula_evaluator_t *eval,
                                const char *formula,
                                const char **variable_names,
                                int variable_count) {
    memset(eval, 0, sizeof(*eval));

    SAFE_STRNCPY(eval->formula, formula, sizeof(eval->formula));
    eval->variable_count = variable_count;

    // Allocate variable storage
    eval->variable_names = calloc(variable_count, sizeof(char*));
    eval->variable_values = calloc(variable_count, sizeof(double));

    if (!eval->variable_names || !eval->variable_values) {
        formula_evaluator_destroy(eval);
        return RESULT_ERROR;
    }

    // Copy variable names
    for (int i = 0; i < variable_count; i++) {
        eval->variable_names[i] = strdup(variable_names[i]);
    }

    // Build te_variable array for TinyExpr
    te_variable *vars = calloc(variable_count, sizeof(te_variable));
    for (int i = 0; i < variable_count; i++) {
        vars[i].name = eval->variable_names[i];
        vars[i].address = &eval->variable_values[i];
        vars[i].type = 0;
        vars[i].context = NULL;
    }

    // Compile expression
    int error_pos;
    eval->te_expr = te_compile(formula, vars, variable_count, &error_pos);

    free(vars);

    if (!eval->te_expr) {
        LOG_ERROR("Formula parse error at position %d: %s", error_pos, formula);
        formula_evaluator_destroy(eval);
        return RESULT_ERROR;
    }

    LOG_INFO("Compiled formula: %s", formula);
    return RESULT_OK;
}

void formula_evaluator_destroy(formula_evaluator_t *eval) {
    if (eval->te_expr) {
        te_free(eval->te_expr);
        eval->te_expr = NULL;
    }

    if (eval->variable_names) {
        for (int i = 0; i < eval->variable_count; i++) {
            if (eval->variable_names[i]) {
                free(eval->variable_names[i]);
            }
        }
        free(eval->variable_names);
        eval->variable_names = NULL;
    }

    if (eval->variable_values) {
        free(eval->variable_values);
        eval->variable_values = NULL;
    }
}

result_t formula_evaluator_evaluate(formula_evaluator_t *eval,
                                    const float *variable_values,
                                    float *result) {
    if (!eval->te_expr) {
        return RESULT_ERROR;
    }

    // Copy input values to double array
    for (int i = 0; i < eval->variable_count; i++) {
        eval->variable_values[i] = (double)variable_values[i];
    }

    // Evaluate expression
    double res = te_eval(eval->te_expr);

    // Check for math errors (NaN, Inf)
    if (isnan(res) || isinf(res)) {
        LOG_ERROR("Formula evaluation error: result is %s",
                 isnan(res) ? "NaN" : "Inf");
        return RESULT_ERROR;
    }

    *result = (float)res;
    return RESULT_OK;
}

#else /* FORMULA_FULL_SUPPORT == 0 */

/* Simple fallback implementation without TinyExpr */

result_t formula_evaluator_init(formula_evaluator_t *eval,
                                const char *formula,
                                const char **variable_names,
                                int variable_count) {
    memset(eval, 0, sizeof(*eval));

    SAFE_STRNCPY(eval->formula, formula, sizeof(eval->formula));
    eval->variable_count = variable_count;

    // Store variable names for simple evaluation
    eval->variable_names = calloc(variable_count, sizeof(char*));
    eval->variable_values = calloc(variable_count, sizeof(double));

    if (!eval->variable_names || !eval->variable_values) {
        formula_evaluator_destroy(eval);
        return RESULT_NO_MEMORY;
    }

    for (int i = 0; i < variable_count; i++) {
        eval->variable_names[i] = strdup(variable_names[i]);
    }

    LOG_INFO("Formula evaluator initialized (simple mode): %s", formula);
    return RESULT_OK;
}

void formula_evaluator_destroy(formula_evaluator_t *eval) {
    if (eval->variable_names) {
        for (int i = 0; i < eval->variable_count; i++) {
            if (eval->variable_names[i]) {
                free(eval->variable_names[i]);
            }
        }
        free(eval->variable_names);
        eval->variable_names = NULL;
    }

    if (eval->variable_values) {
        free(eval->variable_values);
        eval->variable_values = NULL;
    }
}

/* Simple formula evaluator - supports basic operations */
static float simple_eval(const char *formula, const float *values, int count) {
    /* Detect simple patterns */

    /* Average: "(a + b + ...) / N" or "avg(...)" */
    if (strstr(formula, "avg") || (strstr(formula, "+") && strstr(formula, "/"))) {
        float sum = 0.0f;
        for (int i = 0; i < count; i++) {
            sum += values[i];
        }
        return sum / (float)count;
    }

    /* Sum: "a + b + ..." */
    if (strstr(formula, "+") && !strstr(formula, "-") && !strstr(formula, "*") && !strstr(formula, "/")) {
        float sum = 0.0f;
        for (int i = 0; i < count; i++) {
            sum += values[i];
        }
        return sum;
    }

    /* Min */
    if (strstr(formula, "min")) {
        float result = values[0];
        for (int i = 1; i < count; i++) {
            if (values[i] < result) result = values[i];
        }
        return result;
    }

    /* Max */
    if (strstr(formula, "max")) {
        float result = values[0];
        for (int i = 1; i < count; i++) {
            if (values[i] > result) result = values[i];
        }
        return result;
    }

    /* Default: return first value */
    return count > 0 ? values[0] : 0.0f;
}

result_t formula_evaluator_evaluate(formula_evaluator_t *eval,
                                    const float *variable_values,
                                    float *result) {
    if (eval->formula[0] == '\0' || eval->variable_count <= 0) {
        return RESULT_ERROR;
    }

    float res = simple_eval(eval->formula, variable_values, eval->variable_count);

    if (isnan(res) || isinf(res)) {
        LOG_ERROR("Formula evaluation error: result is invalid");
        return RESULT_ERROR;
    }

    *result = res;
    return RESULT_OK;
}

#endif /* FORMULA_FULL_SUPPORT */
