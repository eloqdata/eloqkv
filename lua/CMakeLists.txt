add_library(lua STATIC
        src/fpconv.c src/lapi.c src/lauxlib.c src/lbaselib.c src/lcode.c src/ldblib.c src/ldebug.c src/ldo.c src/ldump.c src/lfunc.c src/lgc.c src/linit.c src/liolib.c src/llex.c src/lmathlib.c src/lmem.c src/loadlib.c src/lobject.c src/lopcodes.c src/loslib.c src/lparser.c src/lstate.c src/lstring.c src/lstrlib.c src/ltable.c src/ltablib.c src/ltm.c src/lua.c src/lua_bit.c src/lua_cjson.c src/lua_cmsgpack.c src/lua_struct.c src/luac.c src/lundump.c src/lvm.c src/lzio.c src/print.c src/strbuf.c
)

add_compile_definitions(ENABLE_CJSON_GLOBAL)

target_compile_options(lua PRIVATE
        -Wno-sign-compare -Wno-misleading-indentation -Wno-implicit-fallthrough -Wno-undefined-inline
        -Wno-stringop-overflow)