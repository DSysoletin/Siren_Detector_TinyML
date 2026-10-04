idf_component_register(SRCS "main.c"
                        "siren_model.cc"
                        "siren_preprocess.c"
                        "audio_capture.c"
                        "siren_net.c"
                        PRIV_REQUIRES esp32_p4_platform
                                     esp_codec_dev
                                     esp-dsp
                                     esp-tflite-micro
                                     freertos
                                     heap
                                     esp_timer
                                     esp_wifi
                                     esp_netif
                                     esp_event
                                     esp_wifi_remote
                                     esp_hosted
                                     mqtt
                        EMBED_FILES "model/siren_detector_model.tflite")
