// belle_config.h -- JFTekWar -> Symbian Belle port: single source of truth for
// the on-device game-data directory (BELLE_GAME_DIR). This is the one place to
// change where the game expects its data (STUFF.DAT sits right there) and where
// tekwar.log / tekwar.ini / savegames land: the engine chdir()s the process into
// this directory in tekgame.c, because the default Symbian process CWD is a
// private folder the user can't browse. Included only on Symbian builds.
#ifndef __belle_config_h__
#define __belle_config_h__

#define BELLE_GAME_DIR "E:/Games/Tekwar"

#endif /* __belle_config_h__ */
