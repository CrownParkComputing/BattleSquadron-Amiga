#ifndef BATTLE_SQUADRON_HYBRIS_LOADER_H
#define BATTLE_SQUADRON_HYBRIS_LOADER_H

#include <stdbool.h>

/* Serve the cracked loader's track reads from the extracted files in
 * `directory` (which must hold disk-map.txt and data/).  No disk image is
 * involved at runtime. */
bool hybris_loader_install(const char *directory);

extern long hybris_load_count;
extern long hybris_load_bytes;
extern long hybris_action_key;
extern long hybris_action_pot;

#endif
