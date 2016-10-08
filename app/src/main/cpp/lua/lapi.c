/*
** $Id: lapi.c,v 2.55.1.5 2008/07/04 18:41:18 roberto Exp $
** Lua API
** See Copyright Notice in lua.h
*/


#include <math.h>
#include <stdarg.h>
#include <string.h>

#define lapi_c
#define LUA_CORE

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lstring.h"
#include "ltable.h"
#include "lundump.h"
#include "lvm.h"


const char lua_ident[] =
                   "$Lua: " LUA_RELEASE " " LUA_COPYRIGHT " $\n"
                           "$Authors: " LUA_AUTHORS " $\n"
                           "$URL: www.lua.org $\n";


#define api_checknelems(L, n)    api_check(L, (n) <= (L->top - L->base))

#define api_checkvalidindex(L, i)    api_check(L, (i) != luaO_nilobject)

#define api_incr_top(L)   {api_check(L, L->top < L->ci->top); L->top++;}


static TValue *index2adr(lua_State *L, int idx) {
    if (idx > 0) {
        TValue *o = L->base + (idx - 1);
        api_check(L, idx <= L->ci->top - L->base);
        if (o >= L->top) return cast(TValue *, luaO_nilobject);
        else return o;
    }
    else if (idx > LUA_REGISTRYINDEX) {
        api_check(L, idx != 0 && -idx <= L->top - L->base);
        return L->top + idx;
    }
    else
        switch (idx) {  /* pseudo-indices */
            case LUA_REGISTRYINDEX:
                return registry(L);
            case LUA_ENVIRONINDEX: {
                Closure *func = curr_func(L);
                sethvalue(L, &L->env, func->c.env);
                return &L->env;
            }
            case LUA_GLOBALSINDEX:
                return gt(L);
            default: {
                Closure *func = curr_func(L);
                idx = LUA_GLOBALSINDEX - idx;
                return (idx <= func->c.nupvalues)
                       ? &func->c.upvalue[idx - 1]
                       : cast(TValue *, luaO_nilobject);
            }
        }
}


static Table *getcurrenv(lua_State *L) {
    if (L->ci == L->base_ci)  /* no enclosing function? */
        return hvalue(gt(L));  /* use global table as environment */
    else {
        Closure *func = curr_func(L);
        return func->c.env;
    }
}


void luaA_pushobject(lua_State *L, const TValue *o) {
    setobj2s(L, L->top, o);
    api_incr_top(L);
}


/**
 * 确保堆栈上至少有 size 个空位。 如果不能把堆栈扩展到相应的尺寸，函数返回 false
 * 这个函数永远不会缩小堆栈； 如果堆栈已经比需要的大了，那么就放在那里不会产生变化
 */
LUA_API int lua_checkstack(lua_State *L, int size) {
    int res = 1;
    lua_lock(L);
    if (size > LUAI_MAXCSTACK || (L->top - L->base + size) > LUAI_MAXCSTACK)
        res = 0;  /* stack overflow */
    else if (size > 0) {
        luaD_checkstack(L, size);
        if (L->ci->top < L->top + size)
            L->ci->top = L->top + size;
    }
    lua_unlock(L);
    return res;
}

/**
 * 传递 同一个 全局状态机下不同线程中的值。
 * 这个函数会从 from 的堆栈中弹出 n 个值， 然后把它们压入 to 的堆栈中。
 */
LUA_API void lua_xmove(lua_State *from, lua_State *to, int n) {
    int i;
    if (from == to) return;
    lua_lock(to);
    api_checknelems(from, n);
    api_check(from, G(from) == G(to));
    api_check(from, to->ci->top - to->top >= n);
    from->top -= n;
    for (i = 0; i < n; i++) {
        setobj2s(to, to->top++, from->top + i);
    }
    lua_unlock(to);
}


LUA_API void lua_setlevel(lua_State *from, lua_State *to) {
    to->nCcalls = from->nCcalls;
}


/**
 * 设置一个新的 panic （恐慌） 函数，并返回前一个。
 * 如果在保护环境之外发生了任何错误， Lua 就会调用一个 panic 函数，接着调用 exit(EXIT_FAILURE)， 这样就开始退出宿主程序。 你的 panic 函数可以永远不返回（例如作一次长跳转）来避免程序退出。
 * panic 函数可以从栈顶取到出错信息
 */
LUA_API lua_CFunction lua_atpanic(lua_State *L, lua_CFunction panicf) {
    lua_CFunction old;
    lua_lock(L);
    old = G(L)->panic;
    G(L)->panic = panicf;
    lua_unlock(L);
    return old;
}

/**
 * 创建一个新线程，并将其压入堆栈， 并返回维护这个线程的 lua_State 指针。 这个函数返回的新状态机共享原有状态机中的所有对象（比如一些 table）， 但是它有独立的执行堆栈。
 * 没有显式的函数可以用来关闭或销毁掉一个线程。 线程跟其它 Lua 对象一样是垃圾收集的条目之一
 */
LUA_API lua_State *lua_newthread(lua_State *L) {
    lua_State *L1;
    lua_lock(L);
    luaC_checkGC(L);
    L1 = luaE_newthread(L);
    setthvalue(L, L->top, L1);
    api_incr_top(L);
    lua_unlock(L);
    luai_userstatethread(L, L1);
    return L1;
}



/*
** basic stack manipulation
*/

/**
 * 返回栈顶元素的索引。因为索引是从 1 开始编号的，所以这个结果等于堆栈上的元素个数（因此返回 0 表示堆栈为空）
 */
LUA_API int lua_gettop(lua_State *L) {
    return cast_int(L->top - L->base);
}

/**
 * 参数允许传入任何可接受的索引以及 0 。它将把堆栈的栈顶设为这个索引。如果新的栈顶比原来的大，超出部分的新元素将被填为nil 。如果index 为 0 ，把栈上所有元素移除
 */
LUA_API void lua_settop(lua_State *L, int idx) {
    lua_lock(L);
    if (idx >= 0) {
        api_check(L, idx <= L->stack_last - L->base);
        while (L->top < L->base + idx)
            setnilvalue(L->top++);
        L->top = L->base + idx;
    }
    else {
        api_check(L, -(idx + 1) <= (L->top - L->base));
        L->top += idx + 1;  /* `subtract' index (index is negative) */
    }
    lua_unlock(L);
}


/**
 * 从给定有效索引处移除一个元素，把这个索引之上的所有元素移下来填补上这个空隙
 */
LUA_API void lua_remove(lua_State *L, int idx) {
    StkId p;
    lua_lock(L);
    p = index2adr(L, idx);
    api_checkvalidindex(L, p);
    while (++p < L->top) setobjs2s (L, p - 1, p);
    L->top--;
    lua_unlock(L);
}

/**
 * 把栈顶元素挪到index位置，原index位置的元素往栈顶移动
 */
LUA_API void lua_insert(lua_State *L, int idx) {
    StkId p;
    StkId q;
    lua_lock(L);
    p = index2adr(L, idx);
    api_checkvalidindex(L, p);
    for (q = L->top; q > p; q--) setobjs2s (L, q, q - 1);
    setobjs2s(L, p, L->top);
    lua_unlock(L);
}

/**
 * 把栈顶元素移动到给定位置（并且把这个栈顶元素弹出），不移动任何元素（因此在那个位置处的值被覆盖掉）
 */
LUA_API void lua_replace(lua_State *L, int idx) {
    StkId o;
    lua_lock(L);
    /* explicit test for incompatible code */
    if (idx == LUA_ENVIRONINDEX && L->ci == L->base_ci)
        luaG_runerror(L, "no calling environment");
    api_checknelems(L, 1);
    o = index2adr(L, idx);
    api_checkvalidindex(L, o);
    if (idx == LUA_ENVIRONINDEX) {
        Closure *func = curr_func(L);
        api_check(L, ttistable(L->top - 1));
        func->c.env = hvalue(L->top - 1);
        luaC_barrier(L, func, L->top - 1);
    }
    else {
        setobj(L, o, L->top - 1);
        if (idx < LUA_GLOBALSINDEX)  /* function upvalue? */
        luaC_barrier(L, curr_func(L), L->top - 1);
    }
    L->top--;
    lua_unlock(L);
}


/**
 * 把堆栈上给定有效处索引处的元素作一个拷贝压栈
 */
LUA_API void lua_pushvalue(lua_State *L, int idx) {
    lua_lock(L);
    setobj2s(L, L->top, index2adr(L, idx));
    api_incr_top(L);
    lua_unlock(L);
}



/*
** access functions (stack -> C)
*/

/**
 * 返回给定索引处的值的类型
 */
LUA_API int lua_type(lua_State *L, int idx) {
    StkId o = index2adr(L, idx);
    return (o == luaO_nilobject) ? LUA_TNONE : ttype(o);
}


/**
 * 返回 t 表示的类型名， 这个 t 必须是 lua_type 可能返回的值中之一
 */
LUA_API const char *lua_typename(lua_State *L, int t) {
    UNUSED(L);
    return (t == LUA_TNONE) ? "no value" : luaT_typenames[t];
}

/**
 * 当给定索引的值是一个 C 函数时，返回 1 ，否则返回 0
 */
LUA_API int lua_iscfunction(lua_State *L, int idx) {
    StkId o = index2adr(L, idx);
    return iscfunction(o);
}

/**
 * 当给定索引的值是一个数字，或是一个可转换为数字的字符串时，返回 1 ，否则返回 0
 */
LUA_API int lua_isnumber(lua_State *L, int idx) {
    TValue       n;
    const TValue *o = index2adr(L, idx);
    return tonumber(o, &n);
}

/**
 * 当给定索引的值是一个字符串或是一个数字（数字总能转换成字符串）时，返回 1 ，否则返回 0
 */
LUA_API int lua_isstring(lua_State *L, int idx) {
    int t = lua_type(L, idx);
    return (t == LUA_TSTRING || t == LUA_TNUMBER);
}

/**
 * 当给定索引的值是一个 userdata （无论是完整的 userdata 还是 light userdata ）时，返回 1 ，否则返回 0
 */
LUA_API int lua_isuserdata(lua_State *L, int idx) {
    const TValue *o = index2adr(L, idx);
    return (ttisuserdata(o) || ttislightuserdata(o));
}

/**
 * 如果两个索引 index1 和 index2 处的值简单地相等 （不调用元方法）则返回 1 。 否则返回 0 。 如果任何一个索引无效也返回 0
 */
LUA_API int lua_rawequal(lua_State *L, int index1, int index2) {
    StkId o1 = index2adr(L, index1);
    StkId o2 = index2adr(L, index2);
    return (o1 == luaO_nilobject || o2 == luaO_nilobject) ? 0
                                                          : luaO_rawequalObj(o1, o2);
}

/**
 * 如果依照 Lua 中 == 操作符语义，索引 index1 和 index2 中的值相同的话，返回 1 。 否则返回 0 。 如果任何一个索引无效也会返回 0
 */
LUA_API int lua_equal(lua_State *L, int index1, int index2) {
    StkId o1, o2;
    int   i;
    lua_lock(L);  /* may call tag method */
    o1 = index2adr(L, index1);
    o2 = index2adr(L, index2);
    i  = (o1 == luaO_nilobject || o2 == luaO_nilobject) ? 0 : equalobj(L, o1, o2);
    lua_unlock(L);
    return i;
}

/**
 * 如果索引 index1 处的值小于 索引 index2 处的值时，返回 1 ； 否则返回 0 。 其语义遵循 Lua 中的 < 操作符（就是说，有可能调用元方法）。 如果任何一个索引无效，也会返回 0
 */
LUA_API int lua_lessthan(lua_State *L, int index1, int index2) {
    StkId o1, o2;
    int   i;
    lua_lock(L);  /* may call tag method */
    o1 = index2adr(L, index1);
    o2 = index2adr(L, index2);
    i  = (o1 == luaO_nilobject || o2 == luaO_nilobject) ? 0
                                                        : luaV_lessthan(L, o1, o2);
    lua_unlock(L);
    return i;
}

/**
 * 把给定索引处的 Lua 值转换为 lua_Number 这样一个 C 类型（参见 lua_Number ）。
 * 这个 Lua 值必须是一个数字或是一个可转换为数字的字符串 （参见 §2.2.1 ）； 否则，lua_tonumber 返回 0
 */
LUA_API lua_Number lua_tonumber(lua_State *L, int idx) {
    TValue       n;
    const TValue *o = index2adr(L, idx);
    if (tonumber(o, &n))
        return nvalue(o);
    else
        return 0;
}

/**
 * 把给定索引处的 Lua 值转换为 lua_Integer 这样一个有符号整数类型。 这个 Lua 值必须是一个数字或是一个可以转换为数字的字符串 （参见 §2.2.1）； 否则，lua_tointeger 返回 0 。
 * 如果数字不是一个整数， 截断小数部分的方式没有被明确定义。
 */
LUA_API lua_Integer lua_tointeger(lua_State *L, int idx) {
    TValue       n;
    const TValue *o = index2adr(L, idx);
    if (tonumber(o, &n)) {
        lua_Integer res;
        lua_Number  num = nvalue(o);
        lua_number2integer(res, num);
        return res;
    }
    else
        return 0;
}


/**
 * 把指定的索引处的的 Lua 值转换为一个 C 中的 boolean 值（ 0 或是 1 ）。
 * 和 Lua 中做的所有测试一样， lua_toboolean 会把任何 不同于 false 和 nil 的值当作 1 返回； 否则就返回 0
 * 如果用一个无效索引去调用也会返回 0（如果你想只接收真正的 boolean 值，就需要使用 lua_isboolean 来测试值的类型。）
 */
LUA_API int lua_toboolean(lua_State *L, int idx) {
    const TValue *o = index2adr(L, idx);
    return !l_isfalse(o);
}

/**
 * 把给定索引处的 Lua 值转换为一个 C 字符串。 如果 len 不为 NULL ， 它还把字符串长度设到 *len 中。 这个 Lua 值必须是一个字符串或是一个数字； 否则返回返回 NULL
 * 如果值是一个数字，lua_tolstring 还会把堆栈中的那个值的实际类型转换为一个字符串。（当遍历一个表的时候，把 lua_tolstring 作用在键上，这个转换有可能导致 lua_next 弄错）
 * lua_tolstring 返回 Lua 状态机中 字符串的以对齐指针。 这个字符串总能保证 （ C 要求的）最后一个字符为零 ('\0') ， 而且它允许在字符串内包含多个这样的零。 因为 Lua 中可能发生垃圾收集， 所以不保证 lua_tolstring 返回的指针， 在对应的值从堆栈中移除后依然有效。
 */
LUA_API const char *lua_tolstring(lua_State *L, int idx, size_t *len) {
    StkId o = index2adr(L, idx);
    if (!ttisstring(o)) {
        lua_lock(L);  /* `luaV_tostring' may create a new string */
        if (!luaV_tostring(L, o)) {  /* conversion failed? */
            if (len != NULL) *len = 0;
            lua_unlock(L);
            return NULL;
        }
        luaC_checkGC(L);
        o = index2adr(L, idx);  /* previous call may reallocate the stack */
        lua_unlock(L);
    }
    if (len != NULL) *len = tsvalue(o)->len;
    return svalue(o);
}


/**
 * 返回指定的索引处的值的长度
 * 对于 string ，那就是字符串的长度； 对于 table ，是取长度操作符 ('#') 的结果； 对于 userdata ，就是为其分配的内存块的尺寸； 对于其它值，为 0
 */
LUA_API size_t lua_objlen(lua_State *L, int idx) {
    StkId o = index2adr(L, idx);
    switch (ttype(o)) {
        case LUA_TSTRING:
            return tsvalue(o)->len;
        case LUA_TUSERDATA:
            return uvalue(o)->len;
        case LUA_TTABLE:
            return luaH_getn(hvalue(o));
        case LUA_TNUMBER: {
            size_t l;
            lua_lock(L);  /* `luaV_tostring' may create a new string */
            l = (luaV_tostring(L, o) ? tsvalue(o)->len : 0);
            lua_unlock(L);
            return l;
        }
        default:
            return 0;
    }
}

/**
 * 把给定索引处的 Lua 值转换为一个 C 函数。 这个值必须是一个 C 函数；如果不是就返回 NULL
 */
LUA_API lua_CFunction lua_tocfunction(lua_State *L, int idx) {
    StkId o = index2adr(L, idx);
    return (!iscfunction(o)) ? NULL : clvalue(o)->c.f;
}

/**
 * 如果给定索引处的值是一个完整的 userdata ，函数返回内存块的地址。 如果值是一个 light userdata ，那么就返回它表示的指针。 否则，返回 NULL
 */
LUA_API void *lua_touserdata(lua_State *L, int idx) {
    StkId o = index2adr(L, idx);
    switch (ttype(o)) {
        case LUA_TUSERDATA:
            return (rawuvalue(o) + 1);
        case LUA_TLIGHTUSERDATA:
            return pvalue(o);
        default:
            return NULL;
    }
}

/**
 * 给定索引处的值转换为一个 Lua 线程（由 lua_State* 代表）。 这个值必须是一个线程；否则函数返回 NULL
 */
LUA_API lua_State *lua_tothread(lua_State *L, int idx) {
    StkId o = index2adr(L, idx);
    return (!ttisthread(o)) ? NULL : thvalue(o);
}

/**
 * 把给定索引处的值转换为一般的 C 指针 (void*) 。 这个值可以是一个 userdata ，table ，thread 或是一个 function ； 否则，lua_topointer 返回 NULL 。 不同的对象有不同的指针。 不存在把指针再转回原有类型的方法
 * 这个函数通常只为产生 debug 信息用
 */
LUA_API const void *lua_topointer(lua_State *L, int idx) {
    StkId o = index2adr(L, idx);
    switch (ttype(o)) {
        case LUA_TTABLE:
            return hvalue(o);
        case LUA_TFUNCTION:
            return clvalue(o);
        case LUA_TTHREAD:
            return thvalue(o);
        case LUA_TUSERDATA:
        case LUA_TLIGHTUSERDATA:
            return lua_touserdata(L, idx);
        default:
            return NULL;
    }
}



/*
** push functions (C -> stack)
*/

/**
 * 把一个 nil 压栈
 */
LUA_API void lua_pushnil(lua_State *L) {
    lua_lock(L);
    setnilvalue(L->top);
    api_incr_top(L);
    lua_unlock(L);
}

/**
 * 把一个数字 n 压栈
 */
LUA_API void lua_pushnumber(lua_State *L, lua_Number n) {
    lua_lock(L);
    setnvalue(L->top, n);
    api_incr_top(L);
    lua_unlock(L);
}

/**
 * 把 n 作为一个数字压栈
 */
LUA_API void lua_pushinteger(lua_State *L, lua_Integer n) {
    lua_lock(L);
    setnvalue(L->top, cast_num(n));
    api_incr_top(L);
    lua_unlock(L);
}

/**
 * 把指针 s 指向的长度为 len 的字符串压栈。 Lua 对这个字符串做一次内存拷贝（或是复用一个拷贝）， 因此 s 处的内存在函数返回后，可以释放掉或是重用于其它用途。 字符串内可以保存有零字符
 */
LUA_API void lua_pushlstring(lua_State *L, const char *s, size_t len) {
    lua_lock(L);
    luaC_checkGC(L);
    setsvalue2s(L, L->top, luaS_newlstr(L, s, len));
    api_incr_top(L);
    lua_unlock(L);
}

/**
 * 把指针 s 指向的以零结尾的字符串压栈。 Lua 对这个字符串做一次内存拷贝（或是复用一个拷贝）， 因此 s 处的内存在函数返回后，可以释放掉或是重用于其它用途。
 * 字符串中不能包含有零字符；第一个碰到的零字符会认为是字符串的结束
 */
LUA_API void lua_pushstring(lua_State *L, const char *s) {
    if (s == NULL)
        lua_pushnil(L);
    else
        lua_pushlstring(L, s, strlen(s));
}

/**
 * 等价于 lua_pushfstring， 不过是用 va_list 接收参数，而不是用可变数量的实际参数
 */
LUA_API const char *lua_pushvfstring(lua_State *L, const char *fmt,
                                     va_list argp) {
    const char *ret;
    lua_lock(L);
    luaC_checkGC(L);
    ret = luaO_pushvfstring(L, fmt, argp);
    lua_unlock(L);
    return ret;
}

/**
 * 把一个格式化过的字符串压入堆栈，然后返回这个字符串的指针。 它和 C 函数 sprintf 比较像，不过有一些重要的区别:
 *     - 摸你需要为结果分配空间： 其结果是一个 Lua 字符串，由 Lua 来关心其内存分配 （同时通过垃圾收集来释放内存）。
 *     - 这个转换非常的受限。 不支持 flag ，宽度，或是指定精度。 它只支持下面这些： '%%' （插入一个 '%'）， '%s' （插入一个带零终止符的字符串，没有长度限制）， '%f' （插入一个 lua_Number）， '%p' （插入一个指针或是一个十六进制数）， '%d' （插入一个 int)， '%c' （把一个 int 作为一个字符插入）
 */
LUA_API const char *lua_pushfstring(lua_State *L, const char *fmt, ...) {
    const char *ret;
    va_list    argp;
    lua_lock(L);
    luaC_checkGC(L);
    va_start(argp, fmt);
    ret = luaO_pushvfstring(L, fmt, argp);
    va_end(argp);
    lua_unlock(L);
    return ret;
}

/**
 * 把一个新的 C closure 压入堆栈
 * 当创建了一个 C 函数后，你可以给它关联一些值，这样就是在创建一个 C closure （参见 §3.4）； 接下来无论函数何时被调用，这些值都可以被这个函数访问到。
 * 为了将一些值关联到一个 C 函数上， 首先这些值需要先被压入堆栈（如果有多个值，第一个先压）。 接下来调用 lua_pushcclosure 来创建出 closure 并把这个 C 函数压到堆栈上。 参数 n 告之函数有多少个值需要关联到函数上。 lua_pushcclosure 也会把这些值从栈上弹出
 */
LUA_API void lua_pushcclosure(lua_State *L, lua_CFunction fn, int n) {
    Closure *cl;
    lua_lock(L);
    luaC_checkGC(L);
    api_checknelems(L, n);
    cl = luaF_newCclosure(L, n, getcurrenv(L));
    cl->c.f = fn;
    L->top -= n;
    while (n--)
            setobj2n (L, &cl->c.upvalue[n], L->top + n);
    setclvalue(L, L->top, cl);
    lua_assert(iswhite(obj2gco(cl)));
    api_incr_top(L);
    lua_unlock(L);
}

/**
 * 把 b 作为一个 boolean 值压入堆栈
 */
LUA_API void lua_pushboolean(lua_State *L, int b) {
    lua_lock(L);
    setbvalue(L->top, (b != 0));  /* ensure that true is 1 */
    api_incr_top(L);
    lua_unlock(L);
}

/**
 * 把一个 light userdata 压栈
 * userdata 在 Lua 中表示一个 C 值。 light userdata 表示一个指针。 它是一个像数字一样的值： 你不需要专门创建它，它也没有独立的 metatable ， 而且也不会被收集（因为从来不需要创建）。 只要表示的 C 地址相同，两个 light userdata 就相等
 */
LUA_API void lua_pushlightuserdata(lua_State *L, void *p) {
    lua_lock(L);
    setpvalue(L->top, p);
    api_incr_top(L);
    lua_unlock(L);
}


/*
 * 把 L 中提供的线程压栈。 如果这个线程是当前状态机的主线程的话，返回 1
 */
LUA_API int lua_pushthread(lua_State *L) {
    lua_lock(L);
    setthvalue(L, L->top, L);
    api_incr_top(L);
    lua_unlock(L);
    return (G(L)->mainthread == L);
}



/*
** get functions (Lua -> stack)
*/


/**
 * 把 t[k] 值压入堆栈，这里的 t 是指有效索引 index 指向的值，而 k 则是栈顶放的值。
 * 这个函数会弹出堆栈上的 key （把结果放在栈上相同位置）。在 Lua 中，这个函数可能触发对应 "index" 事件的元方法
 */
LUA_API void lua_gettable(lua_State *L, int idx) {
    StkId t;
    lua_lock(L);
    t = index2adr(L, idx);
    api_checkvalidindex(L, t);
    luaV_gettable(L, t, L->top - 1, L->top - 1);
    lua_unlock(L);
}


/**
 * 把 t[k] 值压入堆栈，这里的 t 是指有效索引 index 指向的值。在 Lua 中，这个函数可能触发对应 "index" 事件的元方法
 */
LUA_API void lua_getfield(lua_State *L, int idx, const char *k) {
    StkId  t;
    TValue key;
    lua_lock(L);
    t = index2adr(L, idx);
    api_checkvalidindex(L, t);
    setsvalue(L, &key, luaS_new(L, k));
    luaV_gettable(L, t, &key, L->top);
    api_incr_top(L);
    lua_unlock(L);
}

/**
 * 获得idx索引的表中以栈顶为key的值
 */
LUA_API void lua_rawget(lua_State *L, int idx) {
    StkId t;
    lua_lock(L);
    t = index2adr(L, idx);
    api_check(L, ttistable(t));
    setobj2s(L, L->top - 1, luaH_get(hvalue(t), L->top - 1));
    lua_unlock(L);
}

/**
 * 把 t[n] 的值压栈，这里的 t 是指给定索引 index 处的一个值。这是一个直接访问；就是说，它不会触发元方法
 */
LUA_API void lua_rawgeti(lua_State *L, int idx, int n) {
    StkId o;
    lua_lock(L);
    o = index2adr(L, idx);
    api_check(L, ttistable(o));
    setobj2s(L, L->top, luaH_getnum(hvalue(o), n));
    api_incr_top(L);
    lua_unlock(L);
}

/**
 * 创建一个新的空 table 压入堆栈。这个新 table 将被预分配 narr 个元素的数组空间以及 nrec 个元素的非数组空间。
 * 当你明确知道表中需要多少个元素时，预分配就非常有用。如果你不知道，可以使用函数 lua_newtable
 */
LUA_API void lua_createtable(lua_State *L, int narray, int nrec) {
    lua_lock(L);
    luaC_checkGC(L);
    sethvalue(L, L->top, luaH_new(L, narray, nrec));
    api_incr_top(L);
    lua_unlock(L);
}


/**
 * 把给定索引指向的值的元表压入堆栈。如果索引无效，或是这个值没有元表，函数将返回 0 并且不会向栈上压任何东西
 */
LUA_API int lua_getmetatable(lua_State *L, int objindex) {
    const TValue *obj;
    Table        *mt = NULL;
    int          res;
    lua_lock(L);
    obj     = index2adr(L, objindex);
    switch (ttype(obj)) {
        case LUA_TTABLE:
            mt = hvalue(obj)->metatable;
            break;
        case LUA_TUSERDATA:
            mt = uvalue(obj)->metatable;
            break;
        default:
            mt = G(L)->mt[ttype(obj)];
            break;
    }
    if (mt == NULL)
        res = 0;
    else {
        sethvalue(L, L->top, mt);
        api_incr_top(L);
        res = 1;
    }
    lua_unlock(L);
    return res;
}

/**
 * 把索引处值的环境表压入堆栈
 */
LUA_API void lua_getfenv(lua_State *L, int idx) {
    StkId o;
    lua_lock(L);
    o = index2adr(L, idx);
    api_checkvalidindex(L, o);
    switch (ttype(o)) {
        case LUA_TFUNCTION: sethvalue(L, L->top, clvalue(o)->c.env);
            break;
        case LUA_TUSERDATA: sethvalue(L, L->top, uvalue(o)->env);
            break;
        case LUA_TTHREAD:
            setobj2s (L, L->top, gt(thvalue(o)));
            break;
        default:
            setnilvalue(L->top);
            break;
    }
    api_incr_top(L);
    lua_unlock(L);
}


/*
** 以栈顶元素为value，栈顶下一元素为key，设置指定索引的表的值
*/
LUA_API void lua_settable(lua_State *L, int idx) {
    StkId t;
    lua_lock(L);
    api_checknelems(L, 2);
    t = index2adr(L, idx);
    api_checkvalidindex(L, t);
    luaV_settable(L, t, L->top - 2, L->top - 1);
    L->top -= 2;  /* pop index and value */
    lua_unlock(L);
}


/**
 * 做一个等价于 t[k] = v 的操作， 这里 t 是给出的有效索引 index 处的值， 而 v 是栈顶的那个值。
 * 这个函数将把这个值弹出堆栈。 跟在 Lua 中一样，这个函数可能触发一个 "newindex" 事件的元方法
 */
LUA_API void lua_setfield(lua_State *L, int idx, const char *k) {
    StkId  t;
    TValue key;
    lua_lock(L);
    api_checknelems(L, 1);
    t = index2adr(L, idx);
    api_checkvalidindex(L, t);
    setsvalue(L, &key, luaS_new(L, k));
    luaV_settable(L, t, &key, L->top - 1);
    L->top--;  /* pop value */
    lua_unlock(L);
}

/**
 * 设置idx索引的表，添加键值对，以栈顶下一个元素为key,栈顶元素为value
 */
LUA_API void lua_rawset(lua_State *L, int idx) {
    StkId t;
    lua_lock(L);
    api_checknelems(L, 2);
    t = index2adr(L, idx);
    api_check(L, ttistable(t));
    setobj2t(L, luaH_set(L, hvalue(t), L->top - 2), L->top - 1);
    luaC_barriert(L, hvalue(t), L->top - 1);
    L->top -= 2;
    lua_unlock(L);
}

/**
 * 等价于 t[n] = v，这里的 t 是指给定索引 index 处的一个值，而 v 是栈顶的值。
 * 函数将把这个值弹出栈。赋值操作是直接的；就是说，不会触发元方法
 */
LUA_API void lua_rawseti(lua_State *L, int idx, int n) {
    StkId o;
    lua_lock(L);
    api_checknelems(L, 1);
    o = index2adr(L, idx);
    api_check(L, ttistable(o));
    setobj2t(L, luaH_setnum(L, hvalue(o), n), L->top - 1);
    luaC_barriert(L, hvalue(o), L->top - 1);
    L->top--;
    lua_unlock(L);
}

/**
 * 设置栈顶table为指定index的table的metatable
 */
LUA_API int lua_setmetatable(lua_State *L, int objindex) {
    TValue *obj;
    Table  *mt;
    lua_lock(L);
    api_checknelems(L, 1);
    obj = index2adr(L, objindex);
    api_checkvalidindex(L, obj);
    if (ttisnil(L->top - 1))
        mt = NULL;
    else {
        api_check(L, ttistable(L->top - 1));
        mt = hvalue(L->top - 1);
    }
    switch (ttype(obj)) {
        case LUA_TTABLE: {
            hvalue(obj)->metatable = mt;
            if (mt) luaC_objbarriert(L, hvalue(obj), mt);
            break;
        }
        case LUA_TUSERDATA: {
            uvalue(obj)->metatable = mt;
            if (mt) luaC_objbarrier(L, rawuvalue(obj), mt);
            break;
        }
        default: {
            G(L)->mt[ttype(obj)] = mt;
            break;
        }
    }
    L->top--;
    lua_unlock(L);
    return 1;
}

/**
 * 从堆栈上弹出一个 table 并把它设为指定索引处值的新环境
 * 如果指定索引处的值即不是函数又不是线程或是 userdata ， lua_setfenv 会返回 0 ， 否则返回 1 。
 */
LUA_API int lua_setfenv(lua_State *L, int idx) {
    StkId o;
    int   res = 1;
    lua_lock(L);
    api_checknelems(L, 1);
    o = index2adr(L, idx);
    api_checkvalidindex(L, o);
    api_check(L, ttistable(L->top - 1));
    switch (ttype(o)) {
        case LUA_TFUNCTION:
            clvalue(o)->c.env = hvalue(L->top - 1);
            break;
        case LUA_TUSERDATA:
            uvalue(o)->env = hvalue(L->top - 1);
            break;
        case LUA_TTHREAD: sethvalue(L, gt(thvalue(o)), hvalue(L->top - 1));
            break;
        default:
            res = 0;
            break;
    }
    if (res) luaC_objbarrier(L, gcvalue(o), hvalue(L->top - 1));
    L->top--;
    lua_unlock(L);
    return res;
}


/*
** `load' and `call' functions (run Lua code)
*/


#define adjustresults(L, nres) \
    { if (nres == LUA_MULTRET && L->top >= L->ci->top) L->ci->top = L->top; }


#define checkresults(L, na, nr) \
     api_check(L, (nr) == LUA_MULTRET || (L->ci->top - L->top >= (nr) - (na)))

/**
 * 调用一个函数
 * 要调用一个函数请遵循以下协议： 首先，要调用的函数应该被压入堆栈；
 * 接着，把需要传递给这个函数的参数按正序压栈； 这是指第一个参数首先压栈。
 * 最后调用一下 lua_call； nargs 是你压入堆栈的参数个数。
 * 当函数调用完毕后，所有的参数以及函数本身都会出栈。 而函数的返回值这时则被压入堆栈。
 * 返回值的个数将被调整为 nresults 个， 除非 nresults 被设置成 LUA_MULTRET。 在这种情况下，所有的返回值都被压入堆栈中。
 * Lua 会保证返回值都放入栈空间中。 函数返回值将按正序压栈（第一个返回值首先压栈）， 因此在调用结束后，最后一个返回值将被放在栈顶
 */
LUA_API void lua_call(lua_State *L, int nargs, int nresults) {
    StkId func;
    lua_lock(L);
    api_checknelems(L, nargs + 1);
    checkresults(L, nargs, nresults);
    func = L->top - (nargs + 1);
    luaD_call(L, func, nresults);
    adjustresults(L, nresults);
    lua_unlock(L);
}


/*
** Execute a protected call.
*/
struct CallS {
    /* data to `f_call' */
    StkId func;
    int   nresults;
};


static void f_call(lua_State *L, void *ud) {
    struct CallS *c = cast(struct CallS *, ud);
    luaD_call(L, c->func, c->nresults);
}

/**
 * 以保护模式调用一个函数
 * nargs 和 nresults 的含义与 lua_call 中的相同
 */
LUA_API int lua_pcall(lua_State *L, int nargs, int nresults, int errfunc) {
    struct CallS c;
    int          status;
    ptrdiff_t    func;
    lua_lock(L);
    api_checknelems(L, nargs + 1);
    checkresults(L, nargs, nresults);
    if (errfunc == 0)
        func = 0;
    else {
        StkId o = index2adr(L, errfunc);
        api_checkvalidindex(L, o);
        func = savestack(L, o);
    }
    c.func     = L->top - (nargs + 1);  /* function to be called */
    c.nresults = nresults;
    status = luaD_pcall(L, f_call, &c, savestack(L, c.func), func);
    adjustresults(L, nresults);
    lua_unlock(L);
    return status;
}


/*
** Execute a protected C call.
*/
struct CCallS {
    /* data to `f_Ccall' */
    lua_CFunction func;
    void          *ud;
};


static void f_Ccall(lua_State *L, void *ud) {
    struct CCallS *c = cast(struct CCallS *, ud);
    Closure       *cl;
    cl = luaF_newCclosure(L, 0, getcurrenv(L));
    cl->c.f = c->func;
    setclvalue(L, L->top, cl);  /* push function */
    api_incr_top(L);
    setpvalue(L->top, c->ud);  /* push only argument */
    api_incr_top(L);
    luaD_call(L, L->top - 2, 0);
}

/**
 * 以保护模式调用 C 函数 func 。
 * func 只有能从堆栈上拿到一个参数，就是包含有 ud 的 light userdata。
 * 当有错误时， lua_cpcall 返回和 lua_pcall 相同的错误代码， 并在栈顶留下错误对象； 否则它返回零，并不会修改堆栈。所有从 func 内返回的值都会被扔掉
 */
LUA_API int lua_cpcall(lua_State *L, lua_CFunction func, void *ud) {
    struct CCallS c;
    int           status;
    lua_lock(L);
    c.func = func;
    c.ud   = ud;
    status = luaD_pcall(L, f_Ccall, &c, savestack(L, L->top), 0);
    lua_unlock(L);
    return status;
}


LUA_API int lua_load(lua_State *L, lua_Reader reader, void *data,
                     const char *chunkname) {
    ZIO z;
    int status;
    lua_lock(L);
    if (!chunkname) chunkname = "?";
    luaZ_init(L, &z, reader, data);
    status = luaD_protectedparser(L, &z, chunkname);
    lua_unlock(L);
    return status;
}


LUA_API int lua_dump(lua_State *L, lua_Writer writer, void *data) {
    int    status;
    TValue *o;
    lua_lock(L);
    api_checknelems(L, 1);
    o          = L->top - 1;
    if (isLfunction(o))
        status = luaU_dump(L, clvalue(o)->l.p, writer, data, 0);
    else
        status = 1;
    lua_unlock(L);
    return status;
}

/**
 * 返回线程 L 的状态
 */
LUA_API int lua_status(lua_State *L) {
    return L->status;
}


/*
** Garbage-collection function
*/
/**
 * 控制垃圾收集器
 * 这个函数根据其参数 what 发起几种不同的任务：
 * LUA_GCSTOP: 停止垃圾收集器。
 * LUA_GCRESTART: 重启垃圾收集器。
 * LUA_GCCOLLECT: 发起一次完整的垃圾收集循环。
 * LUA_GCCOUNT: 返回 Lua 使用的内存总量（以 K 字节为单位）。
 * LUA_GCCOUNTB: 返回当前内存使用量除以 1024 的余数。
 * LUA_GCSTEP: 发起一步增量垃圾收集。 步数由 data 控制（越大的值意味着越多步）， 而其具体含义（具体数字表示了多少）并未标准化。 如果你想控制这个步数，必须实验性的测试 data 的值。 如果这一步结束了一个垃圾收集周期，返回返回 1 。
 * LUA_GCSETPAUSE: 把 data/100 设置为 garbage-collector pause 的新值（参见 §2.10）。 函数返回以前的值。
 * LUA_GCSETSTEPMUL: 把 arg/100 设置成 step multiplier （参见 §2.10）。 函数返回以前的值。
 */
LUA_API int lua_gc(lua_State *L, int what, int data) {
    int          res = 0;
    global_State *g;
    lua_lock(L);
    g = G(L);
    switch (what) {
        case LUA_GCSTOP: {
            g->GCthreshold = MAX_LUMEM;
            break;
        }
        case LUA_GCRESTART: {
            g->GCthreshold = g->totalbytes;
            break;
        }
        case LUA_GCCOLLECT: {
            luaC_fullgc(L);
            break;
        }
        case LUA_GCCOUNT: {
            /* GC values are expressed in Kbytes: #bytes/2^10 */
            res = cast_int(g->totalbytes >> 10);
            break;
        }
        case LUA_GCCOUNTB: {
            res = cast_int(g->totalbytes & 0x3ff);
            break;
        }
        case LUA_GCSTEP: {
            lu_mem a = (cast(lu_mem, data) << 10);
            if (a <= g->totalbytes)
                g->GCthreshold = g->totalbytes - a;
            else
                g->GCthreshold = 0;
            while (g->GCthreshold <= g->totalbytes) {
                luaC_step(L);
                if (g->gcstate == GCSpause) {  /* end of cycle? */
                    res = 1;  /* signal it */
                    break;
                }
            }
            break;
        }
        case LUA_GCSETPAUSE: {
            res = g->gcpause;
            g->gcpause = data;
            break;
        }
        case LUA_GCSETSTEPMUL: {
            res = g->gcstepmul;
            g->gcstepmul = data;
            break;
        }
        default:
            res = -1;  /* invalid option */
    }
    lua_unlock(L);
    return res;
}



/*
** miscellaneous functions
*/

/**
 * 产生一个 Lua 错误。 错误信息（实际上可以是任何类型的 Lua 值）必须被置入栈顶。 这个函数会做一次长跳转，因此它不会再返回
 */
LUA_API int lua_error(lua_State *L) {
    lua_lock(L);
    api_checknelems(L, 1);
    luaG_errormsg(L);
    lua_unlock(L);
    return 0;  /* to avoid warnings */
}

/**
 * 从栈上弹出一个 key（键）， 然后把索引指定的表中 key-value（健值）对压入堆栈 （指定 key 后面的下一 (next) 对）。 如果表中以无更多元素， 那么 lua_next 将返回 0
 * 典型的遍历方法是这样的：
 *      //table 放在索引 't' 处
 *      lua_pushnil(L);  //第一个 key
 *      while (lua_next(L, t) != 0) {
 *          //用一下 'key' （在索引 -2 处） 和 'value' （在索引 -1 处
 *          printf("%s - %s\n",
 *              lua_typename(L, lua_type(L, -2)),
 *              lua_typename(L, lua_type(L, -1)));
 *          //移除 'value' ；保留 'key' 做下一次迭代
 *          lua_pop(L, 1);
 *      }
 *
 * 在遍历一张表的时候， 不要直接对 key 调用 lua_tolstring ， 除非你知道这个 key 一定是一个字符串。 调用 lua_tolstring 有可能改变给定索引位置的值； 这会对下一次调用 lua_next 造成影响
 */

LUA_API int lua_next(lua_State *L, int idx) {
    StkId t;
    int   more;
    lua_lock(L);
    t = index2adr(L, idx);
    api_check(L, ttistable(t));
    more = luaH_next(L, hvalue(t), L->top - 1);
    if (more) {
        api_incr_top(L);
    }
    else  /* no more elements */
        L->top -= 1;  /* remove key */
    lua_unlock(L);
    return more;
}

/**
 * 连接栈顶的 n 个值， 然后将这些值出栈，并把结果放在栈顶。 如果 n 为 1 ，结果就是一个字符串放在栈上（即，函数什么都不做）； 如果 n 为 0 ，结果是一个空串
 */
LUA_API void lua_concat(lua_State *L, int n) {
    lua_lock(L);
    api_checknelems(L, n);
    if (n >= 2) {
        luaC_checkGC(L);
        luaV_concat(L, n, cast_int(L->top - L->base) - 1);
        L->top -= (n - 1);
    }
    else if (n == 0) {  /* push empty string */
        setsvalue2s(L, L->top, luaS_newlstr(L, "", 0));
        api_incr_top(L);
    }
    /* else n == 1; nothing to do */
    lua_unlock(L);
}

/**
 * 返回给定状态机的内存分配器函数。 如果 ud 不是 NULL ，Lua 把调用 lua_newstate 时传入的那个指针放入 *ud
 */
LUA_API lua_Alloc lua_getallocf(lua_State *L, void **ud) {
    lua_Alloc f;
    lua_lock(L);
    if (ud) *ud = G(L)->ud;
    f = G(L)->frealloc;
    lua_unlock(L);
    return f;
}

/**
 * 把指定状态机的分配器函数换成带上指针 ud 的 f
 */
LUA_API void lua_setallocf(lua_State *L, lua_Alloc f, void *ud) {
    lua_lock(L);
    G(L)->ud       = ud;
    G(L)->frealloc = f;
    lua_unlock(L);
}

/**
 *  函数按照指定的大小分配一块内存，将对应的userdata放到栈内
 */
LUA_API void *lua_newuserdata(lua_State *L, size_t size) {
    Udata *u;
    lua_lock(L);
    luaC_checkGC(L);
    u = luaS_newudata(L, size, getcurrenv(L));
    setuvalue(L, L->top, u);
    api_incr_top(L);
    lua_unlock(L);
    return u + 1;
}


static const char *aux_upvalue(StkId fi, int n, TValue **val) {
    Closure *f;
    if (!ttisfunction(fi)) return NULL;
    f = clvalue(fi);
    if (f->c.isC) {
        if (!(1 <= n && n <= f->c.nupvalues)) return NULL;
        *val = &f->c.upvalue[n - 1];
        return "";
    }
    else {
        Proto *p = f->l.p;
        if (!(1 <= n && n <= p->sizeupvalues)) return NULL;
        *val = f->l.upvals[n - 1]->v;
        return getstr(p->upvalues[n - 1]);
    }
}


LUA_API const char *lua_getupvalue(lua_State *L, int funcindex, int n) {
    const char *name;
    TValue     *val;
    lua_lock(L);
    name = aux_upvalue(index2adr(L, funcindex), n, &val);
    if (name) {
        setobj2s(L, L->top, val);
        api_incr_top(L);
    }
    lua_unlock(L);
    return name;
}


LUA_API const char *lua_setupvalue(lua_State *L, int funcindex, int n) {
    const char *name;
    TValue     *val;
    StkId      fi;
    lua_lock(L);
    fi = index2adr(L, funcindex);
    api_checknelems(L, 1);
    name = aux_upvalue(fi, n, &val);
    if (name) {
        L->top--;
        setobj(L, val, L->top);
        luaC_barrier(L, clvalue(fi), L->top);
    }
    lua_unlock(L);
    return name;
}

