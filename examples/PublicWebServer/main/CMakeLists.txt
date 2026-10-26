idf_component_register(SRCS "sketch.cpp" INCLUDE_DIRS "." REQUIRES hyperdht-arduino
                       PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event
                                     espressif__network_provisioning)
