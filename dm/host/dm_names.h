// What the game calls its things, for the analysis tools (dm_names.c): the model file of each render entity and the file of each texture,
// read from the game's own tables. Read-only.
#pragma once
// Writes a table to path: kind <TAB> key (hex) <TAB> name. kind `model`: key = a render entity's id, name = its model file. kind `texture`:
// key = a texture object or one of its device texture / view pointers, name = the texture file. Returns the rows written, -1 when the game
// has no registry yet or the file cannot be written; counts by kind in *models and *textures (either may be NULL)
int dm_names_dump(const char *path, int *models, int *textures);
