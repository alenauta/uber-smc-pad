/* SMC-PAD flash / VM port for sinco_store (target only), see smcpad_store_port.c. */
#ifndef SMCPAD_STORE_PORT_H
#define SMCPAD_STORE_PORT_H

#include "sinco_store.h"

/* Looks up USRFLASH / USRTRIM once and returns the port (call from the smcpad task). */
const struct sinco_flash_port *smcpad_store_port(void);

#endif
