/* Native save/load assertions, no substitute serialization. GPL-3.0-or-later */
#include "scene_save.h"
#include "filefinder.h"
#include "game_variables.h"
#include "main_data.h"
#include "player.h"
extern "C" bool test_native_save(void)
{
    Main_Data::game_variables->Set(999, 4242);
    return Scene_Save::Save(FileFinder::Save(), 1);
}
extern "C" void test_native_load(void)
{
    Player::LoadSavegame("Save01.lsd", 1);
}
extern "C" bool test_native_loaded(void)
{
    return Main_Data::game_variables->Get(999) == 4242;
}
