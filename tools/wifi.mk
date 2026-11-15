# Portable authentication libraries; no host daemon or target POSIX layer.
include third_party/hostap/sources.mk
WIFI_MBEDTLS_SOURCES := $(wildcard third_party/wifi-mbedtls/library/*.c)
WIFI_AUTH_SOURCES := boards/redmi2/wifi_port.c boards/redmi2/wifi_ec.c boards/redmi2/wifi_auth.c
WIFI_CRYPTO_FLAGS := -Iinclude -Ithird_party/wifi-mbedtls/include \
    -DMBEDTLS_CONFIG_FILE='"wifi/mbedtls_config.h"'
WIFI_AUTH_FLAGS := $(WIFI_CRYPTO_FLAGS) -Ithird_party/hostap/src -Ithird_party/hostap/src/utils \
    -include include/wifi/hostap_config.h \
    -ffunction-sections -fdata-sections -Wno-unused-parameter
build/redmi2-protected-arm32/third_party/hostap/%.o: third_party/hostap/%.c Makefile tools/build.mk tools/wifi.mk include/wifi/hostap_config.h
	@mkdir -p $(dir $@)
	$(ARM32_CC) $(filter-out -Werror,$(REDMI2_PROTECTED_CFLAGS)) $(WIFI_AUTH_FLAGS) -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable -Wno-sign-compare -c $< -o $@
build/redmi2-protected-arm32/third_party/wifi-mbedtls/%.o: third_party/wifi-mbedtls/%.c Makefile tools/build.mk tools/wifi.mk include/wifi/mbedtls_config.h
	@mkdir -p $(dir $@)
	$(ARM32_CC) $(REDMI2_PROTECTED_CFLAGS) $(WIFI_CRYPTO_FLAGS) -Iinclude/wifi/libc -ffunction-sections -fdata-sections -c $< -o $@
build/redmi2-protected-arm32/boards/redmi2/wifi_%.o: boards/redmi2/wifi_%.c Makefile tools/build.mk tools/wifi.mk include/wifi/hostap_config.h include/wifi/mbedtls_config.h
	@mkdir -p $(dir $@)
	$(ARM32_CC) $(REDMI2_PROTECTED_CFLAGS) $(WIFI_AUTH_FLAGS) -c $< -o $@
