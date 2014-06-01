#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <assert.h>
#include <stdlib.h>

#include "ejoy2dgame.h"
#include "fault.h"
#include "shader.h"
#include "texture.h"
#include "ppm.h"
#include "spritepack.h"
#include "sprite.h"
#include "lmatrix.h"
#include "label.h"
#include "particle.h"

//#define LOGIC_FRAME 30

#define EJOY_INIT "EJOY2D_INIT"
#define EJOY_UPDATE "EJOY2D_UPDATE"
#define EJOY_DRAWFRAME "EJOY2D_DRAWFRAME"
#define EJOY_TOUCH "EJOY2D_TOUCH"
#define EJOY_GESTURE "EJOY2D_GESTURE"
#define EJOY_MESSAGE "EJOY2D_MESSAGE"
#define EJOY_HANDLE_ERROR "EJOY2D_HANDLE_ERROR"

#define TRACEBACK_FUNCTION 1
#define UPDATE_FUNCTION 2
#define DRAWFRAME_FUNCTION 3
#define TOP_FUNCTION 3

static int LOGIC_FRAME = 30;

struct game {
	lua_State *L;
	float real_time;
	float logic_time;
};

static int
_panic(lua_State *L) {
	const char * err = lua_tostring(L,-1);
	fault("%s", err);
	return 0;
}

static int
linject(lua_State *L) {
	static const char * ejoy_callback[] = {
		EJOY_INIT,
		EJOY_UPDATE,
		EJOY_DRAWFRAME,
		EJOY_TOUCH,
		EJOY_GESTURE,
		EJOY_MESSAGE,
		EJOY_HANDLE_ERROR,
	};
	int i;
	for (i=0;i<sizeof(ejoy_callback)/sizeof(ejoy_callback[0]);i++) {
		lua_getfield(L, lua_upvalueindex(1), ejoy_callback[i]);
		if (!lua_isfunction(L,-1)) {
			return luaL_error(L, "%s is not found", ejoy_callback[i]);
		}
		lua_setfield(L, LUA_REGISTRYINDEX, ejoy_callback[i]);
	}
	return 0;
}

static int
ejoy2d_framework(lua_State *L) {
	luaL_Reg l[] = {
		{ "inject", linject },
		{ NULL, NULL },
	};
	luaL_newlibtable(L, l);
	lua_pushvalue(L,-1);
	luaL_setfuncs(L,l,1);
	return 1;
}

static void
checkluaversion(lua_State *L) {
	const lua_Number *v = lua_version(L);
	if (v != lua_version(NULL))
		fault("multiple Lua VMs detected");
	else if (*v != LUA_VERSION_NUM) {
		fault("Lua version mismatch: app. needs %f, Lua core provides %f",
			LUA_VERSION_NUM, *v);
	}
}

#define STR_VALUE(arg)	#arg
#define _OS_STRING(name) STR_VALUE(name)
#define OS_STRING _OS_STRING(EJOY2D_OS)

struct game *
ejoy2d_game() {
	struct game *G = (struct game *)malloc(sizeof(*G));
	lua_State *L = luaL_newstate();
	checkluaversion(L);
	lua_pushliteral(L, OS_STRING);
	lua_setglobal(L , "OS");

	G->L = L;
	G->real_time = 0;
	G->logic_time = 0;
	lua_atpanic(L, _panic);
	luaL_openlibs(L);
	luaL_requiref(L, "ejoy2d.shader.c", ejoy2d_shader, 0);
	luaL_requiref(L, "ejoy2d.framework", ejoy2d_framework, 0);
	luaL_requiref(L, "ejoy2d.ppm", ejoy2d_ppm, 0);
	luaL_requiref(L, "ejoy2d.spritepack.c", ejoy2d_spritepack, 0);
	luaL_requiref(L, "ejoy2d.sprite.c", ejoy2d_sprite, 0);
	luaL_requiref(L, "ejoy2d.matrix.c", ejoy2d_matrix, 0);
	luaL_requiref(L, "ejoy2d.particle.c", ejoy2d_particle, 0);

	lua_settop(L,0);

	shader_init();
	label_load();

	return G;
}

void
ejoy2d_game_exit(struct game *G) {
	label_unload();
	texture_exit();
	shader_unload();
	lua_close(G->L);
	free(G);
}

lua_State *
ejoy2d_game_lua(struct game *G) {
	return G->L;
}

static int
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg)
		luaL_traceback(L, L, msg, 1);
	else if (!lua_isnoneornil(L, 1)) {
	if (!luaL_callmeta(L, 1, "__tostring"))
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

void
ejoy2d_game_logicframe(int frame)
{
	LOGIC_FRAME = frame;
}

void
ejoy2d_game_start(struct game *G) {
	lua_State *L = G->L;
	lua_getfield(L, LUA_REGISTRYINDEX, EJOY_INIT);
	lua_call(L, 0, 0);
	assert(lua_gettop(L) == 0);
	lua_pushcfunction(L, traceback);
	lua_getfield(L,LUA_REGISTRYINDEX, EJOY_UPDATE);
	lua_getfield(L,LUA_REGISTRYINDEX, EJOY_DRAWFRAME);
	lua_getfield(L,LUA_REGISTRYINDEX, EJOY_MESSAGE);
}


void 
ejoy2d_handle_error(lua_State *L, const char *err_type, const char *msg) {
	lua_getfield(L, LUA_REGISTRYINDEX, EJOY_HANDLE_ERROR);
	lua_pushstring(L, err_type);
	lua_pushstring(L, msg);
	int err = lua_pcall(L, 2, 0, 0);
	switch(err) {
	case LUA_OK:
		printf("LUA_OK\n");
		break;
	case LUA_ERRRUN:
		printf("LUA_ERRRUN : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRMEM:
		printf("LUA_ERRMEM : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRERR:
		printf("LUA_ERRERR : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRGCMM:
		printf("LUA_ERRGCMM : %s\n", lua_tostring(L,-1));
		break;
	default:
		printf("Unknown Lua error: %d\n", err);
		break;
	}
}

static int
call(lua_State *L, int n, int r) {
	int err = lua_pcall(L, n, r, TRACEBACK_FUNCTION);
	switch(err) {
	case LUA_OK:
		break;
	case LUA_ERRRUN:
		ejoy2d_handle_error(L, "LUA_ERRRUN", lua_tostring(L,-1));
		fault("LUA_ERRRUN : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRMEM:
		ejoy2d_handle_error(L, "LUA_ERRMEM", lua_tostring(L,-1));
		fault("LUA_ERRMEM : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRERR:
		ejoy2d_handle_error(L, "LUA_ERRERR", lua_tostring(L,-1));
		fault("LUA_ERRERR : %s\n", lua_tostring(L,-1));
		break;
	case LUA_ERRGCMM:
		ejoy2d_handle_error(L, "LUA_ERRGCMM", lua_tostring(L,-1));
		fault("LUA_ERRGCMM : %s\n", lua_tostring(L,-1));
		break;
	default:
		ejoy2d_handle_error(L, "UnknownError", "Unknown");
		fault("Unknown Lua error: %d\n", err);
		break;
	}
	return err;
}

static void
logic_frame(lua_State *L) {
	lua_pushvalue(L, UPDATE_FUNCTION);
	call(L, 0, 0);
	lua_settop(L, TOP_FUNCTION);
}

void
ejoy2d_game_update(struct game *G, float time) {
	if (G->logic_time == 0) {
		G->real_time = 1.0f/LOGIC_FRAME;
	} else {
		G->real_time += time;
	}
	while (G->logic_time < G->real_time) {
		logic_frame(G->L);
		G->logic_time += 1.0f/LOGIC_FRAME;
	}
}

void
ejoy2d_game_drawframe(struct game *G, float delta_time) {
	lua_pushvalue(G->L, DRAWFRAME_FUNCTION);
	lua_pushnumber(G->L, delta_time);
	call(G->L, 1, 0);
	lua_settop(G->L, TOP_FUNCTION);
	shader_flush();
	label_flush();
}

int
ejoy2d_game_touch(struct game *G, int id, float x, float y, int status) {
    int disable_gesture = 0;
	lua_getfield(G->L, LUA_REGISTRYINDEX, EJOY_TOUCH);
	lua_pushnumber(G->L, x);
	lua_pushnumber(G->L, y);
	lua_pushinteger(G->L, status+1);
	lua_pushinteger(G->L, id);
	int err = call(G->L, 4, 1);
    if (err == LUA_OK) {
        disable_gesture = lua_toboolean(G->L, -1);
    }
    return disable_gesture;
}

void
ejoy2d_game_gesture(struct game *G, int type,
                    double x1, double y1,double x2,double y2, int s) {
    lua_getfield(G->L, LUA_REGISTRYINDEX, EJOY_GESTURE);
    lua_pushnumber(G->L, type);
    lua_pushnumber(G->L, x1);
    lua_pushnumber(G->L, y1);
    lua_pushnumber(G->L, x2);
    lua_pushnumber(G->L, y2);
    lua_pushinteger(G->L, s);
    call(G->L, 6, 0);
}

void
ejoy2d_game_message(struct game* G,int id_, const char* state, const char* data) {
  lua_State *L = G->L;
  lua_getfield(L, LUA_REGISTRYINDEX, EJOY_MESSAGE);
  lua_pushnumber(L, id_);
  lua_pushstring(L, state);
  lua_pushstring(L, data);
  call(L, 3, 0);
  lua_settop(L, TOP_FUNCTION);
}
