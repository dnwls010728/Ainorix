/* Builds lstate.c with a fixed hash seed.
 * Stock Lua seeds string hashing from time() and addresses, which makes
 * pairs() iteration order differ between runs. OwnEngine promises
 * deterministic simulation, so the seed is a constant here. The Lua
 * sources themselves stay unmodified. */
#define luai_makeseed(L) 0u
#include "lstate.c"
