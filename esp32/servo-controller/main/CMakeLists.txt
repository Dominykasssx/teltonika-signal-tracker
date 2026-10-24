idf_component_register(SRCS "main.c" "controller.c" "flatjson.c" "servo.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver esp_timer)
