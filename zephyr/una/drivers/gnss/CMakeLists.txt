zephyr_library()
zephyr_library_sources_ifdef(CONFIG_GNSS_AG3335M gnss_ag3335m.c)
# The NMEA matching that Zephyr's own GNSS drivers share.
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/gnss)
