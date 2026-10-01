# ============================================================================
# Componente principal do firmware.
#
# As três camadas são compiladas juntas em um único componente ESP-IDF
# (o nome "hal" já é usado por um componente do próprio ESP-IDF):
#
#   app/      lógica da aplicação
#   hal/      camada de abstração de hardware
#   drivers/  controle direto do hardware
#   include/  cabeçalhos (include/drivers, include/hal, include/app)
# ============================================================================
idf_component_register(
    SRC_DIRS
        "."
        "../hal"
        "../drivers"
    INCLUDE_DIRS
        "../include"
        "../include/app"
        "../include/hal"
        "../include/drivers"
    PRIV_REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_uart
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        freertos
        log
        lwip
        nvs_flash
)
