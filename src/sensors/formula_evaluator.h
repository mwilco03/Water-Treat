#ifndef FORMULA_EVALUATOR_H
#define FORMULA_EVALUATOR_H

#include "common.h"

typedef struct {
    char formula[MAX_CONFIG_VALUE_LEN];
    char **variable_names;
    double *variable_values;
    int variable_count;
    void *te_expr;  /* tinyexpr compiled expression */
} formula_evaluator_t;

result_t formula_evaluator_init(formula_evaluator_t *eval,
                                const char *formula,
                                const char **variable_names,
                                int variable_count);
void formula_evaluator_destroy(formula_evaluator_t *eval);
result_t formula_evaluator_evaluate(formula_evaluator_t *eval,
                                    const float *variable_values,
                                    float *result);

#endif
