#ifndef UNA_GNSS_AG3335M_H_
#define UNA_GNSS_AG3335M_H_

#include <stdbool.h>

#include <zephyr/device.h>

/*
 * Power the receiver and start it, or stop it and cut its power. It
 * publishes through Zephyr's GNSS API while it is on. May take seconds.
 */
int gnss_ag3335m_power(const struct device *dev, bool on);

/* Sentences that have come in since the receiver was turned on. */
uint32_t gnss_ag3335m_sentences(const struct device *dev);

#endif
