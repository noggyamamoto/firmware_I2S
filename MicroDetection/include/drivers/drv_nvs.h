/*
 * ============================================================================
 * drivers/drv_nvs – memória flash não volátil (partição NVS)
 * ============================================================================
 */
#ifndef DRV_NVS_H
#define DRV_NVS_H

#include <stdbool.h>
#include <stddef.h>

/** @brief Inicializa a NVS (apaga e recria se a partição estiver corrompida ou desatualizada). */
bool drv_nvs_init(void);

/** @brief Lê uma string. @return false se a chave não existir. */
bool drv_nvs_get_str(const char *ns, const char *key, char *out, size_t max_len);

/** @brief Grava uma string e confirma a escrita (commit). */
bool drv_nvs_set_str(const char *ns, const char *key, const char *value);

#endif // DRV_NVS_H
