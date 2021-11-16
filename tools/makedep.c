/*
 * Generate include file dependencies
 *
 * Copyright 1996, 2013, 2020 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "tools.h"
#include "wine/list.h"

enum incl_type
{
    INCL_NORMAL,           /* #include "foo.h" */
    INCL_SYSTEM,           /* #include <foo.h> */
    INCL_IMPORT,           /* idl import "foo.idl" */
    INCL_IMPORTLIB,        /* idl importlib "foo.tlb" */
    INCL_CPP_QUOTE,        /* idl cpp_quote("#include \"foo.h\"") */
    INCL_CPP_QUOTE_SYSTEM  /* idl cpp_quote("#include <foo.h>") */
};

struct dependency
{
    int                line;          /* source line where this header is included */
    enum incl_type     type;          /* type of include */
    char              *name;          /* header name */
};

struct file
{
    struct list        entry;
    char              *name;          /* full file name relative to cwd */
    void              *args;          /* custom arguments for makefile rule */
    unsigned int       flags;         /* flags (see below) */
    unsigned int       deps_count;    /* files in use */
    unsigned int       deps_size;     /* total allocated size */
    struct dependency *deps;          /* all header dependencies */
};

struct incl_file
{
    struct list        entry;
    struct file       *file;
    char              *name;
    char              *filename;
    char              *basename;      /* base target name for generated files */
    char              *sourcename;    /* source file name for generated headers */
    struct incl_file  *included_by;   /* file that included this one */
    int                included_line; /* line where this file was included */
    enum incl_type     type;          /* type of include */
    int                use_msvcrt:1;  /* put msvcrt headers in the search path? */
    int                is_external:1; /* file from external library? */
    struct incl_file  *owner;
    unsigned int       files_count;   /* files in use */
    unsigned int       files_size;    /* total allocated size */
    struct incl_file **files;
    struct strarray    dependencies;  /* file dependencies */
    struct strarray    importlibdeps; /* importlib dependencies */
};

#define FLAG_GENERATED      0x000001  /* generated file */
#define FLAG_INSTALL        0x000002  /* file to install */
#define FLAG_IDL_PROXY      0x000100  /* generates a proxy (_p.c) file */
#define FLAG_IDL_CLIENT     0x000200  /* generates a client (_c.c) file */
#define FLAG_IDL_SERVER     0x000400  /* generates a server (_s.c) file */
#define FLAG_IDL_IDENT      0x000800  /* generates an ident (_i.c) file */
#define FLAG_IDL_REGISTER   0x001000  /* generates a registration (_r.res) file */
#define FLAG_IDL_TYPELIB    0x002000  /* generates a typelib (_l.res) file */
#define FLAG_IDL_REGTYPELIB 0x004000  /* generates a registered typelib (_t.res) file */
#define FLAG_IDL_HEADER     0x008000  /* generates a header (.h) file */
#define FLAG_RC_PO          0x010000  /* rc file contains translations */
#define FLAG_C_IMPLIB       0x020000  /* file is part of an import library */
#define FLAG_C_UNIX         0x040000  /* file is part of a Unix library */
#define FLAG_SFD_FONTS      0x080000  /* sfd file generated bitmap fonts */

static const struct
{
    unsigned int flag;
    const char *ext;
} idl_outputs[] =
{
    { FLAG_IDL_TYPELIB,    "_l.res" },
    { FLAG_IDL_REGTYPELIB, "_t.res" },
    { FLAG_IDL_CLIENT,     "_c.c" },
    { FLAG_IDL_IDENT,      "_i.c" },
    { FLAG_IDL_PROXY,      "_p.c" },
    { FLAG_IDL_SERVER,     "_s.c" },
    { FLAG_IDL_REGISTER,   "_r.res" },
    { FLAG_IDL_HEADER,     ".h" }
};

#define HASH_SIZE 997

static struct list files[HASH_SIZE];

enum install_rules { INSTALL_LIB, INSTALL_DEV, NB_INSTALL_RULES };

/* variables common to all makefiles */
static struct strarray linguas;
static struct strarray dll_flags;
static struct strarray unix_dllflags;
static struct strarray target_flags;
static struct strarray msvcrt_flags;
static struct strarray extra_cflags;
static struct strarray extra_cross_cflags;
static struct strarray extra_cflags_extlib;
static struct strarray extra_cross_cflags_extlib;
static struct strarray cpp_flags;
static struct strarray lddll_flags;
static struct strarray libs;
static struct strarray enable_tests;
static struct strarray cmdline_vars;
static struct strarray subdirs;
static struct strarray disabled_dirs;
static struct strarray delay_import_libs;
static struct strarray top_install_lib;
static struct strarray top_install_dev;
static const char *root_src_dir;
static const char *tools_dir;
static const char *tools_ext;
static const char *exe_ext;
static const char *dll_ext;
static const char *man_ext;
static const char *host_cpu;
static const char *pe_dir;
static const char *so_dir;
static const char *crosstarget;
static const char *crossdebug;
static const char *fontforge;
static const char *convert;
static const char *flex;
static const char *bison;
static const char *ar;
static const char *ranlib;
static const char *rsvg;
static const char *icotool;
static const char *dlltool;
static const char *msgfmt;
static const char *ln_s;
static const char *sed_cmd;
static const char *delay_load_flag;

struct makefile
{
    /* values determined from input makefile */
    struct strarray vars;
    struct strarray include_paths;
    struct strarray include_args;
    struct strarray define_args;
    struct strarray programs;
    struct strarray scripts;
    struct strarray imports;
    struct strarray delayimports;
    struct strarray extradllflags;
    struct strarray install_lib;
    struct strarray install_dev;
    struct strarray extra_targets;
    struct strarray extra_imports;
    struct list     sources;
    struct list     includes;
    const char     *src_dir;
    const char     *obj_dir;
    const char     *parent_dir;
    const char     *module;
    const char     *testdll;
    const char     *extlib;
    const char     *sharedlib;
    const char     *staticlib;
    const char     *staticimplib;
    const char     *importlib;
    const char     *unixlib;
    int             native_unix_lib;
    int             disabled;
    int             use_msvcrt;
    int             is_cross;
    int             is_win16;
    int             is_exe;

    /* values generated at output time */
    struct strarray in_files;
    struct strarray ok_files;
    struct strarray pot_files;
    struct strarray clean_files;
    struct strarray distclean_files;
    struct strarray uninstall_files;
    struct strarray object_files;
    struct strarray crossobj_files;
    struct strarray unixobj_files;
    struct strarray res_files;
    struct strarray font_files;
    struct strarray c2man_files;
    struct strarray debug_files;
    struct strarray dlldata_files;
    struct strarray implib_objs;
    struct strarray all_targets;
    struct strarray phony_targets;
    struct strarray dependencies;
    struct strarray install_rules[NB_INSTALL_RULES];
};

static struct makefile *top_makefile;
static struct makefile **submakes;

static const char separator[] = "### Dependencies";
static const char *output_makefile_name = "Makefile";
static const char *input_file_name;
static const char *output_file_name;
static const char *temp_file_name;
static int relative_dir_mode;
static int silent_rules;
static int input_line;
static int output_column;
static FILE *output_file;

static const char Usage[] =
    "Usage: makedep [options] [directories]\n"
    "Options:\n"
    "   -R from to  Compute the relative path between two directories\n"
    "   -S          Generate Automake-style silent rules\n"
    "   -fxxx       Store output in file 'xxx' (default: Makefile)\n";


static void fatal_error( const char *msg, ... ) __attribute__ ((__format__ (__printf__, 1, 2)));
static void fatal_perror( const char *msg, ... ) __attribute__ ((__format__ (__printf__, 1, 2)));
static void output( const char *format, ... ) __attribute__ ((__format__ (__printf__, 1, 2)));
static char *strmake( const char* fmt, ... ) __attribute__ ((__format__ (__printf__, 1, 2)));

/*******************************************************************
 *         fatal_error
 */
static void fatal_error( const char *msg, ... )
{
    va_list valist;
    va_start( valist, msg );
    if (input_file_name)
    {
        fprintf( stderr, "%s:", input_file_name );
        if (input_line) fprintf( stderr, "%d:", input_line );
        fprintf( stderr, " error: " );
    }
    else fprintf( stderr, "makedep: error: " );
    vfprintf( stderr, msg, valist );
    va_end( valist );
    exit(1);
}


/*******************************************************************
 *         fatal_perror
 */
static void fatal_perror( const char *msg, ... )
{
    va_list valist;
    va_start( valist, msg );
    if (input_file_name)
    {
        fprintf( stderr, "%s:", input_file_name );
        if (input_line) fprintf( stderr, "%d:", input_line );
        fprintf( stderr, " error: " );
    }
    else fprintf( stderr, "makedep: error: " );
    vfprintf( stderr, msg, valist );
    perror( " " );
    va_end( valist );
    exit(1);
}


/*******************************************************************
 *         cleanup_files
 */
static void cleanup_files(void)
{
    if (temp_file_name) unlink( temp_file_name );
    if (output_file_name) unlink( output_file_name );
}


/*******************************************************************
 *         exit_on_signal
 */
static void exit_on_signal( int sig )
{
    exit( 1 );  /* this will call the atexit functions */
}


/*******************************************************************
 *         output
 */
static void output( const char *format, ... )
{
    int ret;
    va_list valist;

    va_start( valist, format );
    ret = vfprintf( output_file, format, valist );
    va_end( valist );
    if (ret < 0) fatal_perror( "output" );
    if (format[0] && format[strlen(format) - 1] == '\n') output_column = 0;
    else output_column += ret;
}


/*******************************************************************
 *         strarray_get_value
 *
 * Find a value in a name/value pair string array.
 */
static const char *strarray_get_value( const struct strarray *array, const char *name )
{
    int pos, res, min = 0, max = array->count / 2 - 1;

    while (min <= max)
    {
        pos = (min + max) / 2;
        if (!(res = strcmp( array->str[pos * 2], name ))) return array->str[pos * 2 + 1];
        if (res < 0) min = pos + 1;
        else max = pos - 1;
    }
    return NULL;
}


/*******************************************************************
 *         strarray_set_value
 *
 * Define a value in a name/value pair string array.
 */
static void strarray_set_value( struct strarray *array, const char *name, const char *value )
{
    int i, pos, res, min = 0, max = array->count / 2 - 1;

    while (min <= max)
    {
        pos = (min + max) / 2;
        if (!(res = strcmp( array->str[pos * 2], name )))
        {
            /* redefining a variable replaces the previous value */
            array->str[pos * 2 + 1] = value;
            return;
        }
        if (res < 0) min = pos + 1;
        else max = pos - 1;
    }
    strarray_add( array, NULL );
    strarray_add( array, NULL );
    for (i = array->count - 1; i > min * 2 + 1; i--) array->str[i] = array->str[i - 2];
    array->str[min * 2] = name;
    array->str[min * 2 + 1] = value;
}


/*******************************************************************
 *         normalize_arch
 */
static const char *normalize_arch( const char *arch )
{
    unsigned int i, j;

    static const char *map[][8] =
    {
        /* normalized   aliases */
        { "i386",      "i486", "i586", "i686", "ia32" },
        { "x86_64",    "amd64", "x86-64", "x86_amd64", "x64" },
        { "aarch64",   "arm64" },
        { "arm" },
    };

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        for (j = 0; map[i][j]; j++)
            if (!strncmp( arch, map[i][j], strlen(map[i][j]) ))
                return map[i][0];
    return NULL;
}


/*******************************************************************
 *         output_filename
 */
static void output_filename( const char *name )
{
    if (output_column + strlen(name) + 1 > 100)
    {
        output( " \\\n" );
        output( "  " );
    }
    else if (output_column) output( " " );
    output( "%s", name );
}


/*******************************************************************
 *         output_filenames
 */
static void output_filenames( struct strarray array )
{
    unsigned int i;

    for (i = 0; i < array.count; i++) output_filename( array.str[i] );
}


/*******************************************************************
 *         output_rm_filenames
 */
static void output_rm_filenames( struct strarray array )
{
    static const unsigned int max_cmdline = 30000;  /* to be on the safe side */
    unsigned int i, len;

    if (!array.count) return;
    output( "\trm -f" );
    for (i = len = 0; i < array.count; i++)
    {
        if (len > max_cmdline)
        {
            output( "\n" );
            output( "\trm -f" );
            len = 0;
        }
        output_filename( array.str[i] );
        len += strlen( array.str[i] ) + 1;
    }
    output( "\n" );
}


/*******************************************************************
 *         get_extension
 */
static char *get_extension( char *filename )
{
    char *ext = strrchr( filename, '.' );
    if (ext && strchr( ext, '/' )) ext = NULL;
    return ext;
}


/*******************************************************************
 *         get_base_name
 */
static const char *get_base_name( const char *name )
{
    char *base;
    if (!strchr( name, '.' )) return name;
    base = xstrdup( name );
    *strrchr( base, '.' ) = 0;
    return base;
}


/*******************************************************************
 *         replace_filename
 */
static char *replace_filename( const char *path, const char *name )
{
    const char *p;
    char *ret;
    size_t len;

    if (!path) return xstrdup( name );
    if (!(p = strrchr( path, '/' ))) return xstrdup( name );
    len = p - path + 1;
    ret = xmalloc( len + strlen( name ) + 1 );
    memcpy( ret, path, len );
    strcpy( ret + len, name );
    return ret;
}


/*******************************************************************
 *         strarray_replace_extension
 */
static struct strarray strarray_replace_extension( const struct strarray *array,
                                                   const char *old_ext, const char *new_ext )
{
    unsigned int i;
    struct strarray ret;

    ret.count = ret.size = array->count;
    ret.str = xmalloc( sizeof(ret.str[0]) * ret.size );
    for (i = 0; i < array->count; i++) ret.str[i] = replace_extension( array->str[i], old_ext, new_ext );
    return ret;
}


/*******************************************************************
 *         replace_substr
 */
static char *replace_substr( const char *str, const char *start, size_t len, const char *replace )
{
    size_t pos = start - str;
    char *ret = xmalloc( pos + strlen(replace) + strlen(start + len) + 1 );
    memcpy( ret, str, pos );
    strcpy( ret + pos, replace );
    strcat( ret + pos, start + len );
    return ret;
}


/*******************************************************************
 *         get_relative_path
 *
 * Determine where the destination path is located relative to the 'from' path.
 */
static char *get_relative_path( const char *from, const char *dest )
{
    const char *start;
    char *ret, *p;
    unsigned int dotdots = 0;

    /* a path of "." is equivalent to an empty path */
    if (!strcmp( from, "." )) from = "";

    for (;;)
    {
        while (*from == '/') from++;
        while (*dest == '/') dest++;
        start = dest;  /* save start of next path element */
        if (!*from) break;

        while (*from && *from != '/' && *from == *dest) { from++; dest++; }
        if ((!*from || *from == '/') && (!*dest || *dest == '/')) continue;

        /* count remaining elements in 'from' */
        do
        {
            dotdots++;
            while (*from && *from != '/') from++;
            while (*from == '/') from++;
        }
        while (*from);
        break;
    }

    if (!start[0] && !dotdots) return NULL;  /* empty path */

    ret = xmalloc( 3 * dotdots + strlen( start ) + 1 );
    for (p = ret; dotdots; dotdots--, p += 3) memcpy( p, "../", 3 );

    if (start[0]) strcpy( p, start );
    else p[-1] = 0;  /* remove trailing slash */
    return ret;
}


/*******************************************************************
 *         concat_paths
 */
static char *concat_paths( const char *base, const char *path )
{
    int i, len;
    char *ret;

    if (!base || !base[0]) return xstrdup( path && path[0] ? path : "." );
    if (!path || !path[0]) return xstrdup( base );
    if (path[0] == '/') return xstrdup( path );

    len = strlen( base );
    while (len && base[len - 1] == '/') len--;
    while (len && !strncmp( path, "..", 2 ) && (!path[2] || path[2] == '/'))
    {
        for (i = len; i > 0; i--) if (base[i - 1] == '/') break;
        if (i == len - 2 && !memcmp( base + i, "..", 2 )) break;  /* we can't go up if we already have ".." */
        if (i != len - 1 || base[i] != '.')
        {
            path += 2;
            while (*path == '/') path++;
        }
        /* else ignore "." element */
        while (i > 0 && base[i - 1] == '/') i--;
        len = i;
    }
    if (!len && base[0] != '/') return xstrdup( path[0] ? path : "." );
    ret = xmalloc( len + strlen( path ) + 2 );
    memcpy( ret, base, len );
    ret[len++] = '/';
    strcpy( ret + len, path );
    return ret;
}


/*******************************************************************
 *         obj_dir_path
 */
static char *obj_dir_path( const struct makefile *make, const char *path )
{
    return concat_paths( make->obj_dir, path );
}


/*******************************************************************
 *         src_dir_path
 */
static char *src_dir_path( const struct makefile *make, const char *path )
{
    if (make->src_dir) return concat_paths( make->src_dir, path );
    return obj_dir_path( make, path );
}


/*******************************************************************
 *         root_src_dir_path
 */
static char *root_src_dir_path( const char *path )
{
    return concat_paths( root_src_dir, path );
}


/*******************************************************************
 *         tools_dir_path
 */
static char *tools_dir_path( const struct makefile *make, const char *path )
{
    if (tools_dir) return strmake( "%s/tools/%s", tools_dir, path );
    return strmake( "tools/%s", path );
}


/*******************************************************************
 *         tools_path
 */
static char *tools_path( const struct makefile *make, const char *name )
{
    return strmake( "%s/%s%s", tools_dir_path( make, name ), name, tools_ext );
}


/*******************************************************************
 *         strarray_addall_path
 */
static void strarray_addall_path( struct strarray *array, const char *dir, struct strarray added )
{
    unsigned int i;

    for (i = 0; i < added.count; i++) strarray_add( array, concat_paths( dir, added.str[i] ));
}


/*******************************************************************
 *         get_line
 */
static char *get_line( FILE *file )
{
    static char *buffer;
    static size_t size;

    if (!size)
    {
        size = 1024;
        buffer = xmalloc( size );
    }
    if (!fgets( buffer, size, file )) return NULL;
    input_line++;

    for (;;)
    {
        char *p = buffer + strlen(buffer);
        /* if line is larger than buffer, resize buffer */
        while (p == buffer + size - 1 && p[-1] != '\n')
        {
            buffer = xrealloc( buffer, size * 2 );
            if (!fgets( buffer + size - 1, size + 1, file )) break;
            p = buffer + strlen(buffer);
            size *= 2;
        }
        if (p > buffer && p[-1] == '\n')
        {
            *(--p) = 0;
            if (p > buffer && p[-1] == '\r') *(--p) = 0;
            if (p > buffer && p[-1] == '\\')
            {
                *(--p) = 0;
                /* line ends in backslash, read continuation line */
                if (!fgets( p, size - (p - buffer), file )) return buffer;
                input_line++;
                continue;
            }
        }
        return buffer;
    }
}


/*******************************************************************
 *         hash_filename
 */
static unsigned int hash_filename( const char *name )
{
    /* FNV-1 hash */
    unsigned int ret = 2166136261u;
    while (*name) ret = (ret * 16777619) ^ *name++;
    return ret % HASH_SIZE;
}


/*******************************************************************
 *         add_file
 */
static struct file *add_file( const char *name )
{
    struct file *file = xmalloc( sizeof(*file) );
    memset( file, 0, sizeof(*file) );
    file->name = xstrdup( name );
    return file;
}


/*******************************************************************
 *         add_dependency
 */
static void add_dependency( struct file *file, const char *name, enum incl_type type )
{
    if (file->deps_count >= file->deps_size)
    {
        file->deps_size *= 2;
        if (file->deps_size < 16) file->deps_size = 16;
        file->deps = xrealloc( file->deps, file->deps_size * sizeof(*file->deps) );
    }
    file->deps[file->deps_count].line = input_line;
    file->deps[file->deps_count].type = type;
    file->deps[file->deps_count].name = xstrdup( name );
    file->deps_count++;
}


/*******************************************************************
 *         find_src_file
 */
static struct incl_file *find_src_file( const struct makefile *make, const char *name )
{
    struct incl_file *file;

    LIST_FOR_EACH_ENTRY( file, &make->sources, struct incl_file, entry )
        if (!strcmp( name, file->name )) return file;
    return NULL;
}

/*******************************************************************
 *         find_include_file
 */
static struct incl_file *find_include_file( const struct makefile *make, const char *name )
{
    struct incl_file *file;

    LIST_FOR_EACH_ENTRY( file, &make->includes, struct incl_file, entry )
        if (!strcmp( name, file->name )) return file;
    return NULL;
}

/*******************************************************************
 *         add_include
 *
 * Add an include file if it doesn't already exists.
 */
static struct incl_file *add_include( struct makefile *make, struct incl_file *parent,
                                      const char *name, int line, enum incl_type type )
{
    struct incl_file *include;

    if (parent->files_count >= parent->files_size)
    {
        parent->files_size *= 2;
        if (parent->files_size < 16) parent->files_size = 16;
        parent->files = xrealloc( parent->files, parent->files_size * sizeof(*parent->files) );
    }

    LIST_FOR_EACH_ENTRY( include, &make->includes, struct incl_file, entry )
        if (!parent->use_msvcrt == !include->use_msvcrt && !strcmp( name, include->name ))
            goto found;

    include = xmalloc( sizeof(*include) );
    memset( include, 0, sizeof(*include) );
    include->name = xstrdup(name);
    include->included_by = parent;
    include->included_line = line;
    include->type = type;
    include->use_msvcrt = parent->use_msvcrt;
    list_add_tail( &make->includes, &include->entry );
found:
    parent->files[parent->files_count++] = include;
    return include;
}


/*******************************************************************
 *         add_generated_source
 *
 * Add a generated source file to the list.
 */
static struct incl_file *add_generated_source( struct makefile *make,
                                               const char *name, const char *filename )
{
    struct incl_file *file;

    if ((file = find_src_file( make, name ))) return file;  /* we already have it */
    file = xmalloc( sizeof(*file) );
    memset( file, 0, sizeof(*file) );
    file->file = add_file( name );
    file->name = xstrdup( name );
    file->basename = xstrdup( filename ? filename : name );
    file->filename = obj_dir_path( make, file->basename );
    file->file->flags = FLAG_GENERATED;
    file->use_msvcrt = make->use_msvcrt;
    list_add_tail( &make->sources, &file->entry );
    return file;
}


/*******************************************************************
 *         parse_include_directive
 */
static void parse_include_directive( struct file *source, char *str )
{
    char quote, *include, *p = str;

    while (*p && isspace(*p)) p++;
    if (*p != '\"' && *p != '<' ) return;
    quote = *p++;
    if (quote == '<') quote = '>';
    include = p;
    while (*p && (*p != quote)) p++;
    if (!*p) fatal_error( "malformed include directive '%s'\n", str );
    *p = 0;
    add_dependency( source, include, (quote == '>') ? INCL_SYSTEM : INCL_NORMAL );
}


/*******************************************************************
 *         parse_pragma_directive
 */
static void parse_pragma_directive( struct file *source, char *str )
{
    char *flag, *p = str;

    if (!isspace( *p )) return;
    while (*p && isspace(*p)) p++;
    p = strtok( p, " \t" );
    if (strcmp( p, "makedep" )) return;

    while ((flag = strtok( NULL, " \t" )))
    {
        if (!strcmp( flag, "depend" ))
        {
            while ((p = strtok( NULL, " \t" ))) add_dependency( source, p, INCL_NORMAL );
            return;
        }
        else if (!strcmp( flag, "install" )) source->flags |= FLAG_INSTALL;

        if (strendswith( source->name, ".idl" ))
        {
            if (!strcmp( flag, "header" )) source->flags |= FLAG_IDL_HEADER;
            else if (!strcmp( flag, "proxy" )) source->flags |= FLAG_IDL_PROXY;
            else if (!strcmp( flag, "client" )) source->flags |= FLAG_IDL_CLIENT;
            else if (!strcmp( flag, "server" )) source->flags |= FLAG_IDL_SERVER;
            else if (!strcmp( flag, "ident" )) source->flags |= FLAG_IDL_IDENT;
            else if (!strcmp( flag, "typelib" )) source->flags |= FLAG_IDL_TYPELIB;
            else if (!strcmp( flag, "register" )) source->flags |= FLAG_IDL_REGISTER;
            else if (!strcmp( flag, "regtypelib" )) source->flags |= FLAG_IDL_REGTYPELIB;
        }
        else if (strendswith( source->name, ".rc" ))
        {
            if (!strcmp( flag, "po" )) source->flags |= FLAG_RC_PO;
        }
        else if (strendswith( source->name, ".sfd" ))
        {
            if (!strcmp( flag, "font" ))
            {
                struct strarray *array = source->args;

                if (!array)
                {
                    source->args = array = xmalloc( sizeof(*array) );
                    *array = empty_strarray;
                    source->flags |= FLAG_SFD_FONTS;
                }
                strarray_add( array, xstrdup( strtok( NULL, "" )));
                return;
            }
        }
        else
        {
            if (!strcmp( flag, "implib" )) source->flags |= FLAG_C_IMPLIB;
            if (!strcmp( flag, "unix" )) source->flags |= FLAG_C_UNIX;
        }
    }
}


/*******************************************************************
 *         parse_cpp_directive
 */
static void parse_cpp_directive( struct file *source, char *str )
{
    while (*str && isspace(*str)) str++;
    if (*str++ != '#') return;
    while (*str && isspace(*str)) str++;

    if (!strncmp( str, "include", 7 ))
        parse_include_directive( source, str + 7 );
    else if (!strncmp( str, "import", 6 ) && strendswith( source->name, ".m" ))
        parse_include_directive( source, str + 6 );
    else if (!strncmp( str, "pragma", 6 ))
        parse_pragma_directive( source, str + 6 );
}


/*******************************************************************
 *         parse_idl_file
 */
static void parse_idl_file( struct file *source, FILE *file )
{
    char *buffer, *include;

    input_line = 0;

    while ((buffer = get_line( file )))
    {
        char quote;
        char *p = buffer;
        while (*p && isspace(*p)) p++;

        if (!strncmp( p, "importlib", 9 ))
        {
            p += 9;
            while (*p && isspace(*p)) p++;
            if (*p++ != '(') continue;
            while (*p && isspace(*p)) p++;
            if (*p++ != '"') continue;
            include = p;
            while (*p && (*p != '"')) p++;
            if (!*p) fatal_error( "malformed importlib directive\n" );
            *p = 0;
            add_dependency( source, include, INCL_IMPORTLIB );
            continue;
        }

        if (!strncmp( p, "import", 6 ))
        {
            p += 6;
            while (*p && isspace(*p)) p++;
            if (*p != '"') continue;
            include = ++p;
            while (*p && (*p != '"')) p++;
            if (!*p) fatal_error( "malformed import directive\n" );
            *p = 0;
            add_dependency( source, include, INCL_IMPORT );
            continue;
        }

        /* check for #include inside cpp_quote */
        if (!strncmp( p, "cpp_quote", 9 ))
        {
            p += 9;
            while (*p && isspace(*p)) p++;
            if (*p++ != '(') continue;
            while (*p && isspace(*p)) p++;
            if (*p++ != '"') continue;
            if (*p++ != '#') continue;
            while (*p && isspace(*p)) p++;
            if (strncmp( p, "include", 7 )) continue;
            p += 7;
            while (*p && isspace(*p)) p++;
            if (*p == '\\' && p[1] == '"')
            {
                p += 2;
                quote = '"';
            }
            else
            {
                if (*p++ != '<' ) continue;
                quote = '>';
            }
            include = p;
            while (*p && (*p != quote)) p++;
            if (!*p || (quote == '"' && p[-1] != '\\'))
                fatal_error( "malformed #include directive inside cpp_quote\n" );
            if (quote == '"') p--;  /* remove backslash */
            *p = 0;
            add_dependency( source, include, (quote == '>') ? INCL_CPP_QUOTE_SYSTEM : INCL_CPP_QUOTE );
            continue;
        }

        parse_cpp_directive( source, p );
    }
}

/*******************************************************************
 *         parse_c_file
 */
static void parse_c_file( struct file *source, FILE *file )
{
    char *buffer;

    input_line = 0;
    while ((buffer = get_line( file )))
    {
        parse_cpp_directive( source, buffer );
    }
}


/*******************************************************************
 *         parse_rc_file
 */
static void parse_rc_file( struct file *source, FILE *file )
{
    char *buffer, *include;

    input_line = 0;
    while ((buffer = get_line( file )))
    {
        char quote;
        char *p = buffer;
        while (*p && isspace(*p)) p++;

        if (p[0] == '/' && p[1] == '*')  /* check for magic makedep comment */
        {
            p += 2;
            while (*p && isspace(*p)) p++;
            if (strncmp( p, "@makedep:", 9 )) continue;
            p += 9;
            while (*p && isspace(*p)) p++;
            quote = '"';
            if (*p == quote)
            {
                include = ++p;
                while (*p && *p != quote) p++;
            }
            else
            {
                include = p;
                while (*p && !isspace(*p) && *p != '*') p++;
            }
            if (!*p)
                fatal_error( "malformed makedep comment\n" );
            *p = 0;
            add_dependency( source, include, (quote == '>') ? INCL_SYSTEM : INCL_NORMAL );
            continue;
        }

        parse_cpp_directive( source, buffer );
    }
}


/*******************************************************************
 *         parse_in_file
 */
static void parse_in_file( struct file *source, FILE *file )
{
    char *p, *buffer;

    /* make sure it gets rebuilt when the version changes */
    add_dependency( source, "config.h", INCL_SYSTEM );

    if (!strendswith( source->name, ".man.in" )) return;  /* not a man page */

    input_line = 0;
    while ((buffer = get_line( file )))
    {
        if (strncmp( buffer, ".TH", 3 )) continue;
        if (!(p = strtok( buffer, " \t" ))) continue;  /* .TH */
        if (!(p = strtok( NULL, " \t" ))) continue;  /* program name */
        if (!(p = strtok( NULL, " \t" ))) continue;  /* man section */
        source->args = xstrdup( p );
        return;
    }
}


/*******************************************************************
 *         parse_sfd_file
 */
static void parse_sfd_file( struct file *source, FILE *file )
{
    char *p, *eol, *buffer;

    input_line = 0;
    while ((buffer = get_line( file )))
    {
        if (strncmp( buffer, "UComments:", 10 )) continue;
        p = buffer + 10;
        while (*p == ' ') p++;
        if (p[0] == '"' && p[1] && buffer[strlen(buffer) - 1] == '"')
        {
            p++;
            buffer[strlen(buffer) - 1] = 0;
        }
        while ((eol = strstr( p, "+AAoA" )))
        {
            *eol = 0;
            while (*p && isspace(*p)) p++;
            if (*p++ == '#')
            {
                while (*p && isspace(*p)) p++;
                if (!strncmp( p, "pragma", 6 )) parse_pragma_directive( source, p + 6 );
            }
            p = eol + 5;
        }
        while (*p && isspace(*p)) p++;
        if (*p++ != '#') return;
        while (*p && isspace(*p)) p++;
        if (!strncmp( p, "pragma", 6 )) parse_pragma_directive( source, p + 6 );
        return;
    }
}


static const struct
{
    const char *ext;
    void (*parse)( struct file *file, FILE *f );
} parse_functions[] =
{
    { ".c",   parse_c_file },
    { ".h",   parse_c_file },
    { ".inl", parse_c_file },
    { ".l",   parse_c_file },
    { ".m",   parse_c_file },
    { ".rh",  parse_c_file },
    { ".x",   parse_c_file },
    { ".y",   parse_c_file },
    { ".idl", parse_idl_file },
    { ".rc",  parse_rc_file },
    { ".in",  parse_in_file },
    { ".sfd", parse_sfd_file }
};

/*******************************************************************
 *         load_file
 */
static struct file *load_file( const char *name )
{
    struct file *file;
    FILE *f;
    unsigned int i, hash = hash_filename( name );

    LIST_FOR_EACH_ENTRY( file, &files[hash], struct file, entry )
        if (!strcmp( name, file->name )) return file;

    if (!(f = fopen( name, "r" ))) return NULL;

    file = add_file( name );
    list_add_tail( &files[hash], &file->entry );
    input_file_name = file->name;
    input_line = 0;

    for (i = 0; i < sizeof(parse_functions) / sizeof(parse_functions[0]); i++)
    {
        if (!strendswith( name, parse_functions[i].ext )) continue;
        parse_functions[i].parse( file, f );
        break;
    }

    fclose( f );
    input_file_name = NULL;

    return file;
}


/*******************************************************************
 *         open_include_path_file
 *
 * Open a file from a directory on the include path.
 */
static struct file *open_include_path_file( const struct makefile *make, const char *dir,
                                            const char *name, char **filename )
{
    char *src_path = concat_paths( dir, name );
    struct file *ret = load_file( src_path );

    if (ret) *filename = src_path;
    return ret;
}


/*******************************************************************
 *         open_file_same_dir
 *
 * Open a file in the same directory as the parent.
 */
static struct file *open_file_same_dir( const struct incl_file *parent, const char *name, char **filename )
{
    char *src_path = replace_filename( parent->file->name, name );
    struct file *ret = load_file( src_path );

    if (ret) *filename = replace_filename( parent->filename, name );
    return ret;
}


/*******************************************************************
 *         open_local_file
 *
 * Open a file in the source directory of the makefile.
 */
static struct file *open_local_file( const struct makefile *make, const char *path, char **filename )
{
    char *src_path = src_dir_path( make, path );
    struct file *ret = load_file( src_path );

    /* if not found, try parent dir */
    if (!ret && make->parent_dir)
    {
        free( src_path );
        path = strmake( "%s/%s", make->parent_dir, path );
        src_path = src_dir_path( make, path );
        ret = load_file( src_path );
    }

    if (ret) *filename = src_path;
    return ret;
}


/*******************************************************************
 *         open_global_file
 *
 * Open a file in the top-level source directory.
 */
static struct file *open_global_file( const struct makefile *make, const char *path, char **filename )
{
    char *src_path = root_src_dir_path( path );
    struct file *ret = load_file( src_path );

    if (ret) *filename = src_path;
    return ret;
}


/*******************************************************************
 *         open_global_header
 *
 * Open a file in the global include source directory.
 */
static struct file *open_global_header( const struct makefile *make, const char *path, char **filename )
{
    if (!strncmp( path, "../", 3 )) return NULL;
    return open_global_file( make, strmake( "include/%s", path ), filename );
}


/*******************************************************************
 *         open_src_file
 */
static struct file *open_src_file( const struct makefile *make, struct incl_file *pFile )
{
    struct file *file = open_local_file( make, pFile->name, &pFile->filename );

    if (!file) fatal_perror( "open %s", pFile->name );
    return file;
}


/*******************************************************************
 *         find_importlib_module
 */
static struct makefile *find_importlib_module( const char *name )
{
    unsigned int i, len;

    for (i = 0; i < subdirs.count; i++)
    {
        if (strncmp( submakes[i]->obj_dir, "dlls/", 5 )) continue;
        len = strlen(submakes[i]->obj_dir);
        if (strncmp( submakes[i]->obj_dir + 5, name, len - 5 )) continue;
        if (!name[len - 5] || !strcmp( name + len - 5, ".dll" )) return submakes[i];
    }
    return NULL;
}


/*******************************************************************
 *         open_include_file
 */
static struct file *open_include_file( const struct makefile *make, struct incl_file *pFile )
{
    struct file *file = NULL;
    char *filename;
    unsigned int i, len;

    errno = ENOENT;

    /* check for generated bison header */

    if (strendswith( pFile->name, ".tab.h" ) &&
        (file = open_local_file( make, replace_extension( pFile->name, ".tab.h", ".y" ), &filename )))
    {
        pFile->sourcename = filename;
        pFile->filename = obj_dir_path( make, pFile->name );
        return file;
    }

    /* check for corresponding idl file in source dir */

    if (strendswith( pFile->name, ".h" ) &&
        (file = open_local_file( make, replace_extension( pFile->name, ".h", ".idl" ), &filename )))
    {
        pFile->sourcename = filename;
        pFile->filename = obj_dir_path( make, pFile->name );
        return file;
    }

    /* check for extra targets */
    if (strarray_exists( &make->extra_targets, pFile->name ))
    {
        pFile->sourcename = src_dir_path( make, pFile->name );
        pFile->filename = obj_dir_path( make, pFile->name );
        return NULL;
    }

    /* now try in source dir */
    if ((file = open_local_file( make, pFile->name, &pFile->filename ))) return file;

    /* check for global importlib (module dependency) */

    if (pFile->type == INCL_IMPORTLIB && find_importlib_module( pFile->name ))
    {
        pFile->filename = pFile->name;
        return NULL;
    }

    /* check for corresponding idl file in global includes */

    if (strendswith( pFile->name, ".h" ) &&
        (file = open_global_header( make, replace_extension( pFile->name, ".h", ".idl" ), &filename )))
    {
        pFile->sourcename = filename;
        pFile->filename = strmake( "include/%s", pFile->name );
        return file;
    }

    /* check for corresponding .in file in global includes (for config.h.in) */

    if (strendswith( pFile->name, ".h" ) &&
        (file = open_global_header( make, replace_extension( pFile->name, ".h", ".h.in" ), &filename )))
    {
        pFile->sourcename = filename;
        pFile->filename = strmake( "include/%s", pFile->name );
        return file;
    }

    /* check for corresponding .x file in global includes */

    if (strendswith( pFile->name, "tmpl.h" ) &&
        (file = open_global_header( make, replace_extension( pFile->name, ".h", ".x" ), &filename )))
    {
        pFile->sourcename = filename;
        pFile->filename = strmake( "include/%s", pFile->name );
        return file;
    }

    /* check in global includes source dir */

    if ((file = open_global_header( make, pFile->name, &pFile->filename ))) return file;

    /* check in global msvcrt includes */
    if (pFile->use_msvcrt &&
        (file = open_global_header( make, strmake( "msvcrt/%s", pFile->name ), &pFile->filename )))
        return file;

    /* now search in include paths */
    for (i = 0; i < make->include_paths.count; i++)
    {
        const char *dir = make->include_paths.str[i];

        if (root_src_dir)
        {
            len = strlen( root_src_dir );
            if (!strncmp( dir, root_src_dir, len ) && (!dir[len] || dir[len] == '/'))
            {
                while (dir[len] == '/') len++;
                file = open_global_file( make, concat_paths( dir + len, pFile->name ), &pFile->filename );
            }
        }
        else
        {
            if (*dir == '/') continue;
            file = open_include_path_file( make, dir, pFile->name, &pFile->filename );
        }
        if (!file) continue;
        pFile->is_external = 1;
        return file;
    }

    if (pFile->type == INCL_SYSTEM && pFile->use_msvcrt &&
        !make->extlib && !pFile->included_by->is_external)
    {
        if (!strcmp( pFile->name, "stdarg.h" )) return NULL;
        if (!strcmp( pFile->name, "x86intrin.h" )) return NULL;
        if (make->include_paths.count) return NULL;
        fprintf( stderr, "%s:%d: error: system header %s cannot be used with msvcrt\n",
                 pFile->included_by->file->name, pFile->included_line, pFile->name );
        exit(1);
    }

    if (pFile->type == INCL_SYSTEM) return NULL;  /* ignore system files we cannot find */

    /* try in src file directory */
    if ((file = open_file_same_dir( pFile->included_by, pFile->name, &pFile->filename )))
    {
        pFile->is_external = pFile->included_by->is_external;
        return file;
    }

    if (make->extlib) return NULL; /* ignore missing files in external libs */

    fprintf( stderr, "%s:%d: error: ", pFile->included_by->file->name, pFile->included_line );
    perror( pFile->name );
    pFile = pFile->included_by;
    while (pFile && pFile->included_by)
    {
        const char *parent = pFile->included_by->sourcename;
        if (!parent) parent = pFile->included_by->file->name;
        fprintf( stderr, "%s:%d: note: %s was first included here\n",
                 parent, pFile->included_line, pFile->name );
        pFile = pFile->included_by;
    }
    exit(1);
}


/*******************************************************************
 *         add_all_includes
 */
static void add_all_includes( struct makefile *make, struct incl_file *parent, struct file *file )
{
    unsigned int i;

    for (i = 0; i < file->deps_count; i++)
    {
        switch (file->deps[i].type)
        {
        case INCL_NORMAL:
        case INCL_IMPORT:
            add_include( make, parent, file->deps[i].name, file->deps[i].line, INCL_NORMAL );
            break;
        case INCL_IMPORTLIB:
            add_include( make, parent, file->deps[i].name, file->deps[i].line, INCL_IMPORTLIB );
            break;
        case INCL_SYSTEM:
            add_include( make, parent, file->deps[i].name, file->deps[i].line, INCL_SYSTEM );
            break;
        case INCL_CPP_QUOTE:
        case INCL_CPP_QUOTE_SYSTEM:
            break;
        }
    }
}


/*******************************************************************
 *         parse_file
 */
static void parse_file( struct makefile *make, struct incl_file *source, int src )
{
    struct file *file = src ? open_src_file( make, source ) : open_include_file( make, source );

    if (!file) return;

    source->file = file;
    source->files_count = 0;
    source->files_size = file->deps_count;
    source->files = xmalloc( source->files_size * sizeof(*source->files) );
    if (file->flags & FLAG_C_UNIX) source->use_msvcrt = 0;
    else if (file->flags & FLAG_C_IMPLIB) source->use_msvcrt = 1;

    if (source->sourcename)
    {
        if (strendswith( source->sourcename, ".idl" ))
        {
            unsigned int i;

            /* generated .h file always includes these */
            add_include( make, source, "rpc.h", 0, INCL_NORMAL );
            add_include( make, source, "rpcndr.h", 0, INCL_NORMAL );
            for (i = 0; i < file->deps_count; i++)
            {
                switch (file->deps[i].type)
                {
                case INCL_IMPORT:
                    if (strendswith( file->deps[i].name, ".idl" ))
                        add_include( make, source, replace_extension( file->deps[i].name, ".idl", ".h" ),
                                     file->deps[i].line, INCL_NORMAL );
                    else
                        add_include( make, source, file->deps[i].name, file->deps[i].line, INCL_NORMAL );
                    break;
                case INCL_CPP_QUOTE:
                    add_include( make, source, file->deps[i].name, file->deps[i].line, INCL_NORMAL );
                    break;
                case INCL_CPP_QUOTE_SYSTEM:
                    add_include( make, source, file->deps[i].name, file->deps[i].line, INCL_SYSTEM );
                    break;
                case INCL_NORMAL:
                case INCL_SYSTEM:
                case INCL_IMPORTLIB:
                    break;
                }
            }
            return;
        }
        if (strendswith( source->sourcename, ".y" ))
            return;  /* generated .tab.h doesn't include anything */
    }

    add_all_includes( make, source, file );
}


/*******************************************************************
 *         add_src_file
 *
 * Add a source file to the list.
 */
static struct incl_file *add_src_file( struct makefile *make, const char *name )
{
    struct incl_file *file;

    if ((file = find_src_file( make, name ))) return file;  /* we already have it */
    file = xmalloc( sizeof(*file) );
    memset( file, 0, sizeof(*file) );
    file->name = xstrdup(name);
    file->use_msvcrt = make->use_msvcrt;
    file->is_external = !!make->extlib;
    list_add_tail( &make->sources, &file->entry );
    parse_file( make, file, 1 );
    return file;
}


/*******************************************************************
 *         open_input_makefile
 */
static FILE *open_input_makefile( const struct makefile *make )
{
    FILE *ret;

    if (make->obj_dir)
        input_file_name = root_src_dir_path( obj_dir_path( make, "Makefile.in" ));
    else
        input_file_name = output_makefile_name;  /* always use output name for main Makefile */

    input_line = 0;
    if (!(ret = fopen( input_file_name, "r" ))) fatal_perror( "open" );
    return ret;
}


/*******************************************************************
 *         get_make_variable
 */
static const char *get_make_variable( const struct makefile *make, const char *name )
{
    const char *ret;

    if ((ret = strarray_get_value( &cmdline_vars, name ))) return ret;
    if ((ret = strarray_get_value( &make->vars, name ))) return ret;
    if (top_makefile && (ret = strarray_get_value( &top_makefile->vars, name ))) return ret;
    return NULL;
}


/*******************************************************************
 *         get_expanded_make_variable
 */
static char *get_expanded_make_variable( const struct makefile *make, const char *name )
{
    const char *var;
    char *p, *end, *expand, *tmp;

    var = get_make_variable( make, name );
    if (!var) return NULL;

    p = expand = xstrdup( var );
    while ((p = strchr( p, '$' )))
    {
        if (p[1] == '(')
        {
            if (!(end = strchr( p + 2, ')' ))) fatal_error( "syntax error in '%s'\n", expand );
            *end++ = 0;
            if (strchr( p + 2, ':' )) fatal_error( "pattern replacement not supported for '%s'\n", p + 2 );
            var = get_make_variable( make, p + 2 );
            tmp = replace_substr( expand, p, end - p, var ? var : "" );
            /* switch to the new string */
            p = tmp + (p - expand);
            free( expand );
            expand = tmp;
        }
        else if (p[1] == '{')  /* don't expand ${} variables */
        {
            if (!(end = strchr( p + 2, '}' ))) fatal_error( "syntax error in '%s'\n", expand );
            p = end + 1;
        }
        else if (p[1] == '$')
        {
            p += 2;
        }
        else fatal_error( "syntax error in '%s'\n", expand );
    }

    /* consider empty variables undefined */
    p = expand;
    while (*p && isspace(*p)) p++;
    if (*p) return expand;
    free( expand );
    return NULL;
}


/*******************************************************************
 *         get_expanded_make_var_array
 */
static struct strarray get_expanded_make_var_array( const struct makefile *make, const char *name )
{
    struct strarray ret = empty_strarray;
    char *value, *token;

    if ((value = get_expanded_make_variable( make, name )))
        for (token = strtok( value, " \t" ); token; token = strtok( NULL, " \t" ))
            strarray_add( &ret, token );
    return ret;
}


/*******************************************************************
 *         get_expanded_file_local_var
 */
static struct strarray get_expanded_file_local_var( const struct makefile *make, const char *file,
                                                    const char *name )
{
    char *p, *var = strmake( "%s_%s", file, name );

    for (p = var; *p; p++) if (!isalnum( *p )) *p = '_';
    return get_expanded_make_var_array( make, var );
}


/*******************************************************************
 *         set_make_variable
 */
static int set_make_variable( struct strarray *array, const char *assignment )
{
    char *p, *name;

    p = name = xstrdup( assignment );
    while (isalnum(*p) || *p == '_') p++;
    if (name == p) return 0;  /* not a variable */
    if (isspace(*p))
    {
        *p++ = 0;
        while (isspace(*p)) p++;
    }
    if (*p != '=') return 0;  /* not an assignment */
    *p++ = 0;
    while (isspace(*p)) p++;

    strarray_set_value( array, name, p );
    return 1;
}


/*******************************************************************
 *         parse_makefile
 */
static struct makefile *parse_makefile( const char *path )
{
    char *buffer;
    FILE *file;
    struct makefile *make = xmalloc( sizeof(*make) );

    memset( make, 0, sizeof(*make) );
    make->obj_dir = path;
    if (root_src_dir) make->src_dir = root_src_dir_path( make->obj_dir );

    file = open_input_makefile( make );
    while ((buffer = get_line( file )))
    {
        if (!strncmp( buffer, separator, strlen(separator) )) break;
        if (*buffer == '\t') continue;  /* command */
        while (isspace( *buffer )) buffer++;
        if (*buffer == '#') continue;  /* comment */
        set_make_variable( &make->vars, buffer );
    }
    fclose( file );
    input_file_name = NULL;
    return make;
}


/*******************************************************************
 *         add_generated_sources
 */
static void add_generated_sources( struct makefile *make )
{
    unsigned int i;
    struct incl_file *source, *next, *file;
    struct strarray objs = get_expanded_make_var_array( make, "EXTRA_OBJS" );

    LIST_FOR_EACH_ENTRY_SAFE( source, next, &make->sources, struct incl_file, entry )
    {
        if (source->file->flags & FLAG_IDL_CLIENT)
        {
            file = add_generated_source( make, replace_extension( source->name, ".idl", "_c.c" ), NULL );
            add_dependency( file->file, replace_extension( source->name, ".idl", ".h" ), INCL_NORMAL );
            add_all_includes( make, file, file->file );
        }
        if (source->file->flags & FLAG_IDL_SERVER)
        {
            file = add_generated_source( make, replace_extension( source->name, ".idl", "_s.c" ), NULL );
            add_dependency( file->file, "wine/exception.h", INCL_NORMAL );
            add_dependency( file->file, replace_extension( source->name, ".idl", ".h" ), INCL_NORMAL );
            add_all_includes( make, file, file->file );
        }
        if (source->file->flags & FLAG_IDL_IDENT)
        {
            file = add_generated_source( make, replace_extension( source->name, ".idl", "_i.c" ), NULL );
            add_dependency( file->file, "rpc.h", INCL_NORMAL );
            add_dependency( file->file, "rpcndr.h", INCL_NORMAL );
            add_dependency( file->file, "guiddef.h", INCL_NORMAL );
            add_all_includes( make, file, file->file );
        }
        if (source->file->flags & FLAG_IDL_PROXY)
        {
            file = add_generated_source( make, "dlldata.o", "dlldata.c" );
            add_dependency( file->file, "objbase.h", INCL_NORMAL );
            add_dependency( file->file, "rpcproxy.h", INCL_NORMAL );
            add_all_includes( make, file, file->file );
            file = add_generated_source( make, replace_extension( source->name, ".idl", "_p.c" ), NULL );
            add_dependency( file->file, "objbase.h", INCL_NORMAL );
            add_dependency( file->file, "rpcproxy.h", INCL_NORMAL );
            add_dependency( file->file, "wine/exception.h", INCL_NORMAL );
            add_dependency( file->file, replace_extension( source->name, ".idl", ".h" ), INCL_NORMAL );
            add_all_includes( make, file, file->file );
        }
        if (source->file->flags & FLAG_IDL_TYPELIB)
        {
            add_generated_source( make, replace_extension( source->name, ".idl", "_l.res" ), NULL );
        }
        if (source->file->flags & FLAG_IDL_REGTYPELIB)
        {
            add_generated_source( make, replace_extension( source->name, ".idl", "_t.res" ), NULL );
        }
        if (source->file->flags & FLAG_IDL_REGISTER)
        {
            add_generated_source( make, replace_extension( source->name, ".idl", "_r.res" ), NULL );
        }
        if (source->file->flags & FLAG_IDL_HEADER)
        {
            add_generated_source( make, replace_extension( source->name, ".idl", ".h" ), NULL );
        }
        if (!source->file->flags && strendswith( source->name, ".idl" ))
        {
            add_generated_source( make, replace_extension( source->name, ".idl", ".h" ), NULL );
        }
        if (strendswith( source->name, ".x" ))
        {
            add_generated_source( make, replace_extension( source->name, ".x", ".h" ), NULL );
        }
        if (strendswith( source->name, ".y" ))
        {
            file = add_generated_source( make, replace_extension( source->name, ".y", ".tab.c" ), NULL );
            /* steal the includes list from the source file */
            file->files_count = source->files_count;
            file->files_size = source->files_size;
            file->files = source->files;
            source->files_count = source->files_size = 0;
            source->files = NULL;
        }
        if (strendswith( source->name, ".l" ))
        {
            file = add_generated_source( make, replace_extension( source->name, ".l", ".yy.c" ), NULL );
            /* steal the includes list from the source file */
            file->files_count = source->files_count;
            file->files_size = source->files_size;
            file->files = source->files;
            source->files_count = source->files_size = 0;
            source->files = NULL;
        }
        if (source->file->flags & FLAG_C_IMPLIB)
        {
            if (!make->staticimplib && make->importlib && *dll_ext)
                make->staticimplib = strmake( "lib%s.a", make->importlib );
        }
        if (strendswith( source->name, ".po" ))
        {
            if (!make->disabled)
                strarray_add_uniq( &linguas, replace_extension( source->name, ".po", "" ));
        }
        if (strendswith( source->name, ".spec" ))
        {
            char *obj = replace_extension( source->name, ".spec", "" );
            strarray_addall_uniq( &make->extra_imports,
                                  get_expanded_file_local_var( make, obj, "IMPORTS" ));
        }
    }
    if (make->testdll)
    {
        file = add_generated_source( make, "testlist.o", "testlist.c" );
        add_dependency( file->file, "wine/test.h", INCL_NORMAL );
        add_all_includes( make, file, file->file );
    }
    for (i = 0; i < objs.count; i++)
    {
        /* default to .c for unknown extra object files */
        if (strendswith( objs.str[i], ".o" ))
        {
            file = add_generated_source( make, objs.str[i], replace_extension( objs.str[i], ".o", ".c" ));
            file->file->flags |= FLAG_C_UNIX;
            file->use_msvcrt = 0;
        }
        else if (strendswith( objs.str[i], ".res" ))
            add_generated_source( make, replace_extension( objs.str[i], ".res", ".rc" ), NULL );
        else
            add_generated_source( make, objs.str[i], NULL );
    }
}


/*******************************************************************
 *         create_dir
 */
static void create_dir( const char *dir )
{
    char *p, *path;

    p = path = xstrdup( dir );
    while ((p = strchr( p, '/' )))
    {
        *p = 0;
        if (mkdir( path, 0755 ) == -1 && errno != EEXIST) fatal_perror( "mkdir %s", path );
        *p++ = '/';
        while (*p == '/') p++;
    }
    if (mkdir( path, 0755 ) == -1 && errno != EEXIST) fatal_perror( "mkdir %s", path );
    free( path );
}


/*******************************************************************
 *         create_file_directories
 *
 * Create the base directories of all the files.
 */
static void create_file_directories( const struct makefile *make, struct strarray files )
{
    struct strarray subdirs = empty_strarray;
    unsigned int i;
    char *dir;

    for (i = 0; i < files.count; i++)
    {
        if (!strchr( files.str[i], '/' )) continue;
        dir = obj_dir_path( make, files.str[i] );
        *strrchr( dir, '/' ) = 0;
        strarray_add_uniq( &subdirs, dir );
    }

    for (i = 0; i < subdirs.count; i++) create_dir( subdirs.str[i] );
}


/*******************************************************************
 *         output_filenames_obj_dir
 */
static void output_filenames_obj_dir( const struct makefile *make, struct strarray array )
{
    unsigned int i;

    for (i = 0; i < array.count; i++) output_filename( obj_dir_path( make, array.str[i] ));
}


/*******************************************************************
 *         get_dependencies
 */
static void get_dependencies( struct incl_file *file, struct incl_file *source )
{
    unsigned int i;

    if (!file->filename) return;

    if (file != source)
    {
        if (file->owner == source) return;  /* already processed */
        if (file->type == INCL_IMPORTLIB)
        {
            if (!(source->file->flags & (FLAG_IDL_TYPELIB | FLAG_IDL_REGTYPELIB)))
                return;  /* library is imported only when building a typelib */
            strarray_add( &source->importlibdeps, file->filename );
        }
        else strarray_add( &source->dependencies, file->filename );
        file->owner = source;

        /* sanity checks */
        if (!strcmp( file->filename, "include/config.h" ) &&
            file != source->files[0] && !source->is_external)
        {
            input_file_name = source->filename;
            input_line = 0;
            for (i = 0; i < source->file->deps_count; i++)
            {
                if (!strcmp( source->file->deps[i].name, file->name ))
                    input_line = source->file->deps[i].line;
            }
            fatal_error( "%s must be included before other headers\n", file->name );
        }
    }

    for (i = 0; i < file->files_count; i++) get_dependencies( file->files[i], source );
}


/*******************************************************************
 *         get_local_dependencies
 *
 * Get the local dependencies of a given target.
 */
static struct strarray get_local_dependencies( const struct makefile *make, const char *name,
                                               struct strarray targets )
{
    unsigned int i;
    struct strarray deps = get_expanded_file_local_var( make, name, "DEPS" );

    for (i = 0; i < deps.count; i++)
    {
        if (strarray_exists( &targets, deps.str[i] ))
            deps.str[i] = obj_dir_path( make, deps.str[i] );
        else
            deps.str[i] = src_dir_path( make, deps.str[i] );
    }
    return deps;
}


/*******************************************************************
 *         get_static_lib
 *
 * Check if makefile builds the named static library and return the full lib path.
 */
static const char *get_static_lib( const struct makefile *make, const char *name )
{
    if (!make->staticlib) return NULL;
    if (make->disabled) return NULL;
    if (strncmp( make->staticlib, "lib", 3 )) return NULL;
    if (strncmp( make->staticlib + 3, name, strlen(name) )) return NULL;
    if (strcmp( make->staticlib + 3 + strlen(name), ".a" )) return NULL;
    return obj_dir_path( make, make->staticlib );
}


/*******************************************************************
 *         get_native_unix_lib
 */
static const char *get_native_unix_lib( const struct makefile *make, const char *name )
{
    if (!make->native_unix_lib) return NULL;
    if (strncmp( make->unixlib, name, strlen(name) )) return NULL;
    if (make->unixlib[strlen(name)] != '.') return NULL;
    return obj_dir_path( make, make->unixlib );
}


/*******************************************************************
 *         get_parent_makefile
 */
static struct makefile *get_parent_makefile( struct makefile *make )
{
    char *dir, *p;
    int i;

    if (!make->obj_dir) return NULL;
    dir = xstrdup( make->obj_dir );
    if (!(p = strrchr( dir, '/' ))) return NULL;
    *p = 0;
    for (i = 0; i < subdirs.count; i++)
        if (!strcmp( submakes[i]->obj_dir, dir )) return submakes[i];
    return NULL;
}


/*******************************************************************
 *         needs_delay_lib
 */
static int needs_delay_lib( const struct makefile *make )
{
    if (delay_load_flag) return 0;
    if (*dll_ext && !crosstarget) return 0;
    if (!make->importlib) return 0;
    return strarray_exists( &delay_import_libs, make->importlib );
}


/*******************************************************************
 *         needs_implib_symlink
 */
static int needs_implib_symlink( const struct makefile *make )
{
    if (!make->module) return 0;
    if (!make->importlib) return 0;
    if (make->is_win16 && make->disabled) return 0;
    if (strncmp( make->obj_dir, "dlls/", 5 )) return 0;
    if (!strcmp( make->module, make->importlib )) return 0;
    if (!strchr( make->importlib, '.' ) &&
        !strncmp( make->module, make->importlib, strlen( make->importlib )) &&
        !strcmp( make->module + strlen( make->importlib ), ".dll" ))
        return 0;
    return 1;
}


/*******************************************************************
 *         add_unix_libraries
 */
static struct strarray add_unix_libraries( const struct makefile *make, struct strarray *deps )
{
    struct strarray ret = empty_strarray;
    struct strarray all_libs = empty_strarray;
    unsigned int i, j;

    if (make->native_unix_lib && strcmp( make->unixlib, "ntdll.so" )) strarray_add( &all_libs, "-lntdll" );
    strarray_addall( &all_libs, get_expanded_make_var_array( make, "EXTRALIBS" ));
    strarray_addall( &all_libs, libs );

    for (i = 0; i < all_libs.count; i++)
    {
        const char *lib = NULL;

        if (!strncmp( all_libs.str[i], "-l", 2 ))
        {
            const char *name = all_libs.str[i] + 2;

            for (j = 0; j < subdirs.count; j++)
            {
                if (make == submakes[j]) continue;
                if ((lib = get_static_lib( submakes[j], name ))) break;
            }
            if (!lib && make->native_unix_lib)
                for (j = 0; j < subdirs.count; j++)
                {
                    if (make == submakes[j]) continue;
                    if ((lib = get_native_unix_lib( submakes[j], name ))) break;
                }
        }

        if (lib)
        {
            strarray_add( deps, lib );
            strarray_add( &ret, lib );
        }
        else strarray_add( &ret, all_libs.str[i] );
    }
    return ret;
}


/*******************************************************************
 *         is_crt_module
 */
static int is_crt_module( const char *file )
{
    return !strncmp( file, "msvcr", 5 ) || !strncmp( file, "ucrt", 4 ) || !strcmp( file, "crtdll.dll" );
}


/*******************************************************************
 *         get_default_crt
 */
static const char *get_default_crt( const struct makefile *make )
{
    if (!make->use_msvcrt) return NULL;
    if (make->module && is_crt_module( make->module )) return NULL;  /* don't add crt import to crt dlls */
    return !make->testdll && (!make->staticlib || make->extlib) ? "ucrtbase" : "msvcrt";
}


/*******************************************************************
 *         get_crt_define
 */
static const char *get_crt_define( const struct makefile *make )
{
    const char *crt_dll = NULL;
    unsigned int i, version = 0;

    for (i = 0; i < make->imports.count; i++)
    {
        if (!is_crt_module( make->imports.str[i] )) continue;
        if (crt_dll) fatal_error( "More than one C runtime DLL imported: %s and %s\n",
                                  crt_dll, make->imports.str[i] );
        crt_dll = make->imports.str[i];
    }

    if (!crt_dll)
    {
        if (strarray_exists( &make->extradllflags, "-nodefaultlibs" )) return "-D_MSVCR_VER=0";
        if (!(crt_dll = get_default_crt( make ))) crt_dll = make->module;
    }
    if (!strncmp( crt_dll, "ucrt", 4 )) return "-D_UCRT";
    sscanf( crt_dll, "msvcr%u", &version );
    return strmake( "-D_MSVCR_VER=%u", version );
}


/*******************************************************************
 *         add_default_imports
 */
static struct strarray add_default_imports( const struct makefile *make, struct strarray imports )
{
    struct strarray ret = empty_strarray;
    const char *crt_dll = get_default_crt( make );
    unsigned int i;

    for (i = 0; i < imports.count; i++)
    {
        if (is_crt_module( imports.str[i] )) crt_dll = imports.str[i];
        else strarray_add( &ret, imports.str[i] );
    }

    strarray_add( &ret, "winecrt0" );
    if (crt_dll) strarray_add( &ret, crt_dll );

    if (make->is_win16 && (!make->importlib || strcmp( make->importlib, "kernel" )))
        strarray_add( &ret, "kernel" );

    strarray_add( &ret, "kernel32" );
    strarray_add( &ret, "ntdll" );
    return ret;
}


/*******************************************************************
 *         add_import_libs
 */
static struct strarray add_import_libs( const struct makefile *make, struct strarray *deps,
                                        struct strarray imports, int delay, int is_unix )
{
    struct strarray ret = empty_strarray;
    unsigned int i, j;
    int is_cross = make->is_cross && !is_unix;

    if (!delay && !is_unix && !strarray_exists( &make->extradllflags, "-nodefaultlibs" ))
        imports = add_default_imports( make, imports );

    for (i = 0; i < imports.count; i++)
    {
        const char *name = imports.str[i];
        const char *lib = NULL;

        if (name[0] == '-')
        {
            switch (name[1])
            {
            case 'L': strarray_add( &ret, name ); continue;
            case 'l': name += 2; break;
            default: continue;
            }
        }
        else name = get_base_name( name );

        for (j = 0; j < subdirs.count; j++)
        {
            if (submakes[j]->importlib && !strcmp( submakes[j]->importlib, name ))
                lib = obj_dir_path( submakes[j], strmake( "lib%s.a", name ));
            else
                lib = get_static_lib( submakes[j], name );
            if (lib) break;
        }

        if (lib)
        {
            const char *ext = NULL;

            if (delay && !delay_load_flag && (is_cross || !*dll_ext)) ext = ".delay.a";
            else if (is_cross) ext = ".cross.a";
            if (ext) lib = replace_extension( lib, ".a", ext );
            strarray_add_uniq( deps, lib );
            strarray_add( &ret, lib );
            if (needs_implib_symlink( submakes[j] ))
                strarray_add_uniq( deps, strmake( "dlls/lib%s%s", name, ext ? ext : ".a" ));
        }
        else strarray_add( &ret, strmake( "-l%s", name ));
    }
    return ret;
}


/*******************************************************************
 *         add_install_rule
 */
static void add_install_rule( struct makefile *make, const char *target,
                              const char *file, const char *dest )
{
    if (strarray_exists( &make->install_lib, target ) ||
        strarray_exists( &top_install_lib, make->obj_dir ) ||
        strarray_exists( &top_install_lib, obj_dir_path( make, target )))
    {
        strarray_add( &make->install_rules[INSTALL_LIB], file );
        strarray_add( &make->install_rules[INSTALL_LIB], dest );
    }
    else if (strarray_exists( &make->install_dev, target ) ||
             strarray_exists( &top_install_dev, make->obj_dir ) ||
             strarray_exists( &top_install_dev, obj_dir_path( make, target )))
    {
        strarray_add( &make->install_rules[INSTALL_DEV], file );
        strarray_add( &make->install_rules[INSTALL_DEV], dest );
    }
}


/*******************************************************************
 *         get_include_install_path
 *
 * Determine the installation path for a given include file.
 */
static const char *get_include_install_path( const char *name )
{
    if (!strncmp( name, "wine/", 5 )) return name + 5;
    if (!strncmp( name, "msvcrt/", 7 )) return name;
    return strmake( "windows/%s", name );
}


/*******************************************************************
 *         get_shared_library_name
 *
 * Determine possible names for a shared library with a version number.
 */
static struct strarray get_shared_lib_names( const char *libname )
{
    struct strarray ret = empty_strarray;
    const char *ext, *p;
    char *name, *first, *second;
    size_t len = 0;

    strarray_add( &ret, libname );

    for (p = libname; (p = strchr( p, '.' )); p++)
        if ((len = strspn( p + 1, "0123456789." ))) break;

    if (!len) return ret;
    ext = p + 1 + len;
    if (*ext && ext[-1] == '.') ext--;

    /* keep only the first group of digits */
    name = xstrdup( libname );
    first = name + (p - libname);
    if ((second = strchr( first + 1, '.' )))
    {
        strcpy( second, ext );
        strarray_add( &ret, xstrdup( name ));
    }
    return ret;
}


/*******************************************************************
 *         get_source_defines
 */
static struct strarray get_source_defines( struct makefile *make, struct incl_file *source,
                                           const char *obj )
{
    unsigned int i;
    struct strarray ret = empty_strarray;

    strarray_addall( &ret, make->include_args );
    if (source->use_msvcrt)
        strarray_add( &ret, strmake( "-I%s", root_src_dir_path( "include/msvcrt" )));
    for (i = 0; i < make->include_paths.count; i++)
        strarray_add( &ret, strmake( "-I%s", make->include_paths.str[i] ));
    strarray_addall( &ret, make->define_args );
    strarray_addall( &ret, get_expanded_file_local_var( make, obj, "EXTRADEFS" ));
    if ((source->file->flags & FLAG_C_UNIX) && *dll_ext) strarray_add( &ret, "-DWINE_UNIX_LIB" );
    return ret;
}


/*******************************************************************
 *         remove_warning_flags
 */
static struct strarray remove_warning_flags( struct strarray flags )
{
    unsigned int i;
    struct strarray ret = empty_strarray;

    for (i = 0; i < flags.count; i++)
        if (strncmp( flags.str[i], "-W", 2 ) || !strncmp( flags.str[i], "-Wno-", 5 ))
            strarray_add( &ret, flags.str[i] );
    return ret;
}


/*******************************************************************
 *         get_debug_file
 */
static const char *get_debug_file( struct makefile *make, const char *name )
{
    const char *debug_file = NULL;
    if (!make->is_cross || !crossdebug) return NULL;
    if (!strcmp( crossdebug, "pdb" )) debug_file = strmake( "%s.pdb", get_base_name( name ));
    else if(!strncmp( crossdebug, "split", 5 )) debug_file = strmake( "%s.debug", name );
    if (debug_file) strarray_add( &make->debug_files, debug_file );
    return debug_file;
}


/*******************************************************************
 *         cmd_prefix
 */
static const char *cmd_prefix( const char *cmd )
{
    if (!silent_rules) return "";
    return strmake( "$(quiet_%s)", cmd );
}


/*******************************************************************
 *         output_winegcc_command
 */
static void output_winegcc_command( struct makefile *make, int is_cross )
{
    output( "\t%s%s -o $@", cmd_prefix( "CCLD" ), tools_path( make, "winegcc" ));
    output_filename( "--wine-objdir ." );
    if (tools_dir)
    {
        output_filename( "--winebuild" );
        output_filename( tools_path( make, "winebuild" ));
    }
    if (is_cross)
    {
        output_filename( "-b" );
        output_filename( crosstarget );
        output_filename( "--lib-suffix=.cross.a" );
    }
    else
    {
        output_filenames( target_flags );
        output_filenames( lddll_flags );
    }
}


/*******************************************************************
 *         output_symlink_rule
 *
 * Output a rule to create a symlink.
 */
static void output_symlink_rule( const char *src_name, const char *link_name, int create_dir )
{
    const char *name = strrchr( link_name, '/' );
    char *dir = NULL;

    if (name)
    {
        dir = xstrdup( link_name );
        dir[name - link_name] = 0;
    }

    output( "\t%s", cmd_prefix( "LN" ));
    if (create_dir && dir && *dir) output( "%s -d %s && ", root_src_dir_path( "tools/install-sh" ), dir );
    output( "rm -f %s && ", link_name );

    /* dest path with a directory needs special handling if ln -s isn't supported */
    if (dir && strcmp( ln_s, "ln -s" ))
        output( "cd %s && %s %s %s\n", *dir ? dir : "/", ln_s, src_name, name + 1 );
    else
        output( "%s %s %s\n", ln_s, src_name, link_name );

    free( dir );
}


/*******************************************************************
 *         output_srcdir_symlink
 *
 * Output rule to create a symlink back to the source directory, for source files
 * that are needed at run-time.
 */
static void output_srcdir_symlink( struct makefile *make, const char *obj )
{
    char *src_file, *dst_file, *src_name;

    if (!make->src_dir) return;
    src_file = src_dir_path( make, obj );
    dst_file = obj_dir_path( make, obj );
    output( "%s: %s\n", dst_file, src_file );

    src_name = src_file;
    if (src_name[0] != '/' && make->obj_dir)
        src_name = concat_paths( get_relative_path( make->obj_dir, "" ), src_name );

    output_symlink_rule( src_name, dst_file, 0 );
    strarray_add( &make->all_targets, obj );
}


/*******************************************************************
 *         output_install_commands
 */
static void output_install_commands( struct makefile *make, struct strarray files )
{
    unsigned int i;
    char *install_sh = root_src_dir_path( "tools/install-sh" );

    for (i = 0; i < files.count; i += 2)
    {
        const char *file = files.str[i];
        const char *dest = strmake( "$(DESTDIR)%s", files.str[i + 1] + 1 );

        switch (*files.str[i + 1])
        {
        case 'c':  /* cross-compiled program */
            output( "\tSTRIPPROG=%s-strip %s -m 644 $(INSTALL_PROGRAM_FLAGS) %s %s\n",
                    crosstarget, install_sh, obj_dir_path( make, file ), dest );
            output( "\t%s --builtin %s\n", tools_path( make, "winebuild" ), dest );
            break;
        case 'd':  /* data file */
            output( "\t%s -m 644 $(INSTALL_DATA_FLAGS) %s %s\n",
                    install_sh, obj_dir_path( make, file ), dest );
            break;
        case 'D':  /* data file in source dir */
            output( "\t%s -m 644 $(INSTALL_DATA_FLAGS) %s %s\n",
                    install_sh, src_dir_path( make, file ), dest );
            break;
        case 'p':  /* program file */
            output( "\tSTRIPPROG=\"$(STRIP)\" %s $(INSTALL_PROGRAM_FLAGS) %s %s\n",
                    install_sh, obj_dir_path( make, file ), dest );
            break;
        case 's':  /* script */
            output( "\t%s $(INSTALL_SCRIPT_FLAGS) %s %s\n",
                    install_sh, obj_dir_path( make, file ), dest );
            break;
        case 'S':  /* script in source dir */
            output( "\t%s $(INSTALL_SCRIPT_FLAGS) %s %s\n",
                    install_sh, src_dir_path( make, file ), dest );
            break;
        case 't':  /* script in tools dir */
            output( "\t%s $(INSTALL_SCRIPT_FLAGS) %s %s\n",
                    install_sh, tools_dir_path( make, files.str[i] ), dest );
            break;
        case 'y':  /* symlink */
            output_symlink_rule( files.str[i], dest, 1 );
            break;
        default:
            assert(0);
        }
        strarray_add( &make->uninstall_files, dest );
    }
}


/*******************************************************************
 *         output_install_rules
 *
 * Rules are stored as a (file,dest) pair of values.
 * The first char of dest indicates the type of install.
 */
static void output_install_rules( struct makefile *make, enum install_rules rules, const char *target )
{
    unsigned int i;
    struct strarray files = make->install_rules[rules];
    struct strarray targets = empty_strarray;

    if (!files.count) return;

    for (i = 0; i < files.count; i += 2)
    {
        const char *file = files.str[i];
        switch (*files.str[i + 1])
        {
        case 'c':  /* cross-compiled program */
        case 'd':  /* data file */
        case 'p':  /* program file */
        case 's':  /* script */
            strarray_add_uniq( &targets, obj_dir_path( make, file ));
            break;
        case 't':  /* script in tools dir */
            strarray_add_uniq( &targets, tools_dir_path( make, file ));
            break;
        }
    }

    output( "%s %s::", obj_dir_path( make, "install" ), obj_dir_path( make, target ));
    output_filenames( targets );
    output( "\n" );
    output_install_commands( make, files );
    strarray_add_uniq( &make->phony_targets, obj_dir_path( make, "install" ));
    strarray_add_uniq( &make->phony_targets, obj_dir_path( make, target ));
}


static int cmp_string_length( const char **a, const char **b )
{
    int paths_a = 0, paths_b = 0;
    const char *p;

    for (p = *a; *p; p++) if (*p == '/') paths_a++;
    for (p = *b; *p; p++) if (*p == '/') paths_b++;
    if (paths_b != paths_a) return paths_b - paths_a;
    return strcmp( *a, *b );
}

/*******************************************************************
 *         output_uninstall_rules
 */
static void output_uninstall_rules( struct makefile *make )
{
    static const char *dirs_order[] =
        { "$(includedir)", "$(mandir)", "$(fontdir)", "$(nlsdir)", "$(datadir)", "$(dlldir)" };

    struct strarray uninstall_dirs = empty_strarray;
    unsigned int i, j;

    if (!make->uninstall_files.count) return;
    output( "uninstall::\n" );
    output_rm_filenames( make->uninstall_files );
    strarray_add_uniq( &make->phony_targets, "uninstall" );

    if (!subdirs.count) return;
    for (i = 0; i < make->uninstall_files.count; i++)
    {
        char *dir = xstrdup( make->uninstall_files.str[i] );
        while (strchr( dir, '/' ))
        {
            *strrchr( dir, '/' ) = 0;
            strarray_add_uniq( &uninstall_dirs, xstrdup(dir) );
        }
    }
    strarray_qsort( &uninstall_dirs, cmp_string_length );
    output( "\t-rmdir" );
    for (i = 0; i < sizeof(dirs_order)/sizeof(dirs_order[0]); i++)
    {
        for (j = 0; j < uninstall_dirs.count; j++)
        {
            if (!uninstall_dirs.str[j]) continue;
            if (strncmp( uninstall_dirs.str[j] + strlen("$(DESTDIR)"), dirs_order[i], strlen(dirs_order[i]) ))
                continue;
            output_filename( uninstall_dirs.str[j] );
            uninstall_dirs.str[j] = NULL;
        }
    }
    for (j = 0; j < uninstall_dirs.count; j++)
        if (uninstall_dirs.str[j]) output_filename( uninstall_dirs.str[j] );
    output( "\n" );
}


/*******************************************************************
 *         output_importlib_symlinks
 */
static struct strarray output_importlib_symlinks( const struct makefile *make )
{
    struct strarray ret = empty_strarray;
    const char *lib, *dst, *ext[2] = { "a", "cross.a" };
    int i, count = 1 + !!crosstarget;

    for (i = 0; i < count; i++)
    {
        lib = strmake( "lib%s.%s", make->importlib, ext[i] );
        dst = strmake( "dlls/%s", lib );
        output( "%s: %s\n", dst, obj_dir_path( make, lib ));
        output_symlink_rule( concat_paths( make->obj_dir + strlen("dlls/"), lib ), dst, 0 );
        strarray_add( &ret, dst );
    }
    return ret;
}


/*******************************************************************
 *         output_po_files
 */
static void output_po_files( const struct makefile *make )
{
    const char *po_dir = src_dir_path( make, "po" );
    unsigned int i;

    if (linguas.count)
    {
        for (i = 0; i < linguas.count; i++)
            output_filename( strmake( "%s/%s.po", po_dir, linguas.str[i] ));
        output( ": %s/wine.pot\n", po_dir );
        output( "\t%smsgmerge --previous -q $@ %s/wine.pot | msgattrib --no-obsolete -o $@.new && mv $@.new $@\n",
                cmd_prefix( "MSG" ), po_dir );
        output( "po:" );
        for (i = 0; i < linguas.count; i++)
            output_filename( strmake( "%s/%s.po", po_dir, linguas.str[i] ));
        output( "\n" );
    }
    output( "%s/wine.pot:", po_dir );
    output_filenames( make->pot_files );
    output( "\n" );
    output( "\t%smsgcat -o $@", cmd_prefix( "MSG" ));
    output_filenames( make->pot_files );
    output( "\n" );
}


/*******************************************************************
 *         output_source_y
 */
static void output_source_y( struct makefile *make, struct incl_file *source, const char *obj )
{
    /* add source file dependency for parallel makes */
    char *header = strmake( "%s.tab.h", obj );

    if (find_include_file( make, header ))
    {
        output( "%s: %s\n", obj_dir_path( make, header ), source->filename );
        output( "\t%s%s -p %s_ -o %s.tab.c -d %s\n",
                cmd_prefix( "BISON" ), bison, obj, obj_dir_path( make, obj ), source->filename );
        output( "%s.tab.c: %s %s\n", obj_dir_path( make, obj ),
                source->filename, obj_dir_path( make, header ));
        strarray_add( &make->clean_files, header );
    }
    else output( "%s.tab.c: %s\n", obj_dir_path( make, obj ), source->filename );

    output( "\t%s%s -p %s_ -o $@ %s\n", cmd_prefix( "BISON" ), bison, obj, source->filename );
}


/*******************************************************************
 *         output_source_l
 */
static void output_source_l( struct makefile *make, struct incl_file *source, const char *obj )
{
    output( "%s.yy.c: %s\n", obj_dir_path( make, obj ), source->filename );
    output( "\t%s%s -o$@ %s\n", cmd_prefix( "FLEX" ), flex, source->filename );
}


/*******************************************************************
 *         output_source_h
 */
static void output_source_h( struct makefile *make, struct incl_file *source, const char *obj )
{
    if (source->file->flags & FLAG_GENERATED)
        strarray_add( &make->all_targets, source->name );
    else
        add_install_rule( make, source->name, source->name,
                          strmake( "D$(includedir)/wine/%s", get_include_install_path( source->name ) ));
}


/*******************************************************************
 *         output_source_rc
 */
static void output_source_rc( struct makefile *make, struct incl_file *source, const char *obj )
{
    struct strarray defines = get_source_defines( make, source, obj );
    const char *po_dir = NULL;
    unsigned int i;

    if (source->file->flags & FLAG_GENERATED) strarray_add( &make->clean_files, source->name );
    if (linguas.count && (source->file->flags & FLAG_RC_PO)) po_dir = "po";
    strarray_add( &make->res_files, strmake( "%s.res", obj ));
    if (source->file->flags & FLAG_RC_PO)
    {
        strarray_add( &make->pot_files, strmake( "%s.pot", obj ));
        output( "%s.pot ", obj_dir_path( make, obj ) );
    }
    output( "%s.res: %s", obj_dir_path( make, obj ), source->filename );
    output_filename( tools_path( make, "wrc" ));
    output_filenames( source->dependencies );
    output( "\n" );
    output( "\t%s%s -u -o $@", cmd_prefix( "WRC" ), tools_path( make, "wrc" ) );
    if (make->is_win16) output_filename( "-m16" );
    output_filename( "--nostdinc" );
    if (po_dir) output_filename( strmake( "--po-dir=%s", po_dir ));
    output_filenames( defines );
    output_filename( source->filename );
    output( "\n" );
    if (po_dir)
    {
        output( "%s.res:", obj_dir_path( make, obj ));
        for (i = 0; i < linguas.count; i++)
            output_filename( strmake( "%s/%s.mo", po_dir, linguas.str[i] ));
        output( "\n" );
    }
}


/*******************************************************************
 *         output_source_mc
 */
static void output_source_mc( struct makefile *make, struct incl_file *source, const char *obj )
{
    unsigned int i;
    char *obj_path = obj_dir_path( make, obj );

    strarray_add( &make->res_files, strmake( "%s.res", obj ));
    strarray_add( &make->pot_files, strmake( "%s.pot", obj ));
    output( "%s.pot %s.res: %s", obj_path, obj_path, source->filename );
    output_filename( tools_path( make, "wmc" ));
    output_filenames( source->dependencies );
    output( "\n" );
    output( "\t%s%s -u -o $@ %s", cmd_prefix( "WMC" ), tools_path( make, "wmc" ), source->filename );
    if (linguas.count)
    {
        output_filename( "--po-dir=po" );
        output( "\n" );
        output( "%s.res:", obj_dir_path( make, obj ));
        for (i = 0; i < linguas.count; i++)
            output_filename( strmake( "po/%s.mo", linguas.str[i] ));
    }
    output( "\n" );
}


/*******************************************************************
 *         output_source_res
 */
static void output_source_res( struct makefile *make, struct incl_file *source, const char *obj )
{
    strarray_add( &make->res_files, source->name );
}


/*******************************************************************
 *         output_source_idl
 */
static void output_source_idl( struct makefile *make, struct incl_file *source, const char *obj )
{
    struct strarray defines = get_source_defines( make, source, obj );
    struct strarray targets = empty_strarray;
    char *dest;
    unsigned int i;

    if (!source->file->flags) source->file->flags |= FLAG_IDL_HEADER | FLAG_INSTALL;
    if (find_include_file( make, strmake( "%s.h", obj ))) source->file->flags |= FLAG_IDL_HEADER;

    for (i = 0; i < sizeof(idl_outputs) / sizeof(idl_outputs[0]); i++)
    {
        if (!(source->file->flags & idl_outputs[i].flag)) continue;
        dest = strmake( "%s%s", obj, idl_outputs[i].ext );
        if (!find_src_file( make, dest )) strarray_add( &make->clean_files, dest );
        strarray_add( &targets, dest );
    }
    if (source->file->flags & FLAG_IDL_PROXY) strarray_add( &make->dlldata_files, source->name );
    if (source->file->flags & FLAG_INSTALL)
    {
        add_install_rule( make, source->name, xstrdup( source->name ),
                          strmake( "D$(includedir)/wine/%s.idl", get_include_install_path( obj ) ));
        if (source->file->flags & FLAG_IDL_HEADER)
            add_install_rule( make, source->name, strmake( "%s.h", obj ),
                              strmake( "d$(includedir)/wine/%s.h", get_include_install_path( obj ) ));
    }
    if (!targets.count) return;

    output_filenames_obj_dir( make, targets );
    output( ": %s\n", tools_path( make, "widl" ));
    output( "\t%s%s -o $@", cmd_prefix( "WIDL" ), tools_path( make, "widl" ) );
    output_filenames( target_flags );
    output_filename( "--nostdinc" );
    output_filename( "-Ldlls/\\*" );
    output_filenames( defines );
    output_filenames( get_expanded_make_var_array( make, "EXTRAIDLFLAGS" ));
    output_filenames( get_expanded_file_local_var( make, obj, "EXTRAIDLFLAGS" ));
    output_filename( source->filename );
    output( "\n" );
    output_filenames_obj_dir( make, targets );
    output( ": %s", source->filename );
    output_filenames( source->dependencies );
    for (i = 0; i < source->importlibdeps.count; i++)
    {
        struct makefile *submake = find_importlib_module( source->importlibdeps.str[i] );
        const char *module = submake->module;
        if (*dll_ext && !submake->is_cross) module = strmake( "%s.fake", module );
        output_filename( obj_dir_path( submake, module ));
    }
    output( "\n" );
}


/*******************************************************************
 *         output_source_x
 */
static void output_source_x( struct makefile *make, struct incl_file *source, const char *obj )
{
    output( "%s.h: %s%s %s\n", obj_dir_path( make, obj ),
            tools_dir_path( make, "make_xftmpl" ), tools_ext, source->filename );
    output( "\t%s%s%s -H -o $@ %s\n", cmd_prefix( "GEN" ),
            tools_dir_path( make, "make_xftmpl" ), tools_ext, source->filename );
    if (source->file->flags & FLAG_INSTALL)
    {
        add_install_rule( make, source->name, source->name,
                          strmake( "D$(includedir)/wine/%s", get_include_install_path( source->name ) ));
        add_install_rule( make, source->name, strmake( "%s.h", obj ),
                          strmake( "d$(includedir)/wine/%s.h", get_include_install_path( obj ) ));
    }
}


/*******************************************************************
 *         output_source_sfd
 */
static void output_source_sfd( struct makefile *make, struct incl_file *source, const char *obj )
{
    unsigned int i;
    char *ttf_obj = strmake( "%s.ttf", obj );
    char *ttf_file = src_dir_path( make, ttf_obj );

    if (fontforge && !make->src_dir)
    {
        output( "%s: %s\n", ttf_file, source->filename );
        output( "\t%s%s -script %s %s $@\n", cmd_prefix( "GEN" ),
                fontforge, root_src_dir_path( "fonts/genttf.ff" ), source->filename );
        if (!(source->file->flags & FLAG_SFD_FONTS)) strarray_add( &make->font_files, ttf_obj );
    }
    if (source->file->flags & FLAG_INSTALL)
    {
        add_install_rule( make, source->name, ttf_obj, strmake( "D$(fontdir)/%s", ttf_obj ));
        output_srcdir_symlink( make, ttf_obj );
    }

    if (source->file->flags & FLAG_SFD_FONTS)
    {
        struct strarray *array = source->file->args;

        for (i = 0; i < array->count; i++)
        {
            char *font = strtok( xstrdup(array->str[i]), " \t" );
            char *args = strtok( NULL, "" );

            strarray_add( &make->all_targets, xstrdup( font ));
            output( "%s: %s %s\n", obj_dir_path( make, font ),
                    tools_path( make, "sfnt2fon" ), ttf_file );
            output( "\t%s%s -q -o $@ %s %s\n", cmd_prefix( "GEN" ),
                    tools_path( make, "sfnt2fon" ), ttf_file, args );
            add_install_rule( make, source->name, xstrdup(font), strmake( "d$(fontdir)/%s", font ));
        }
    }
}


/*******************************************************************
 *         output_source_svg
 */
static void output_source_svg( struct makefile *make, struct incl_file *source, const char *obj )
{
    static const char * const images[] = { "bmp", "cur", "ico", NULL };
    unsigned int i;

    if (convert && rsvg && icotool && !make->src_dir)
    {
        for (i = 0; images[i]; i++)
            if (find_include_file( make, strmake( "%s.%s", obj, images[i] ))) break;

        if (images[i])
        {
            output( "%s.%s: %s\n", src_dir_path( make, obj ), images[i], source->filename );
            output( "\t%sCONVERT=\"%s\" ICOTOOL=\"%s\" RSVG=\"%s\" %s %s $@\n",
                    cmd_prefix( "GEN" ), convert, icotool, rsvg,
                    root_src_dir_path( "tools/buildimage" ), source->filename );
        }
    }
}


/*******************************************************************
 *         output_source_nls
 */
static void output_source_nls( struct makefile *make, struct incl_file *source, const char *obj )
{
    add_install_rule( make, source->name, source->name,
                      strmake( "D$(nlsdir)/%s", source->name ));
    output_srcdir_symlink( make, strmake( "%s.nls", obj ));
}


/*******************************************************************
 *         output_source_desktop
 */
static void output_source_desktop( struct makefile *make, struct incl_file *source, const char *obj )
{
    add_install_rule( make, source->name, source->name,
                      strmake( "D$(datadir)/applications/%s", source->name ));
}


/*******************************************************************
 *         output_source_po
 */
static void output_source_po( struct makefile *make, struct incl_file *source, const char *obj )
{
    output( "%s.mo: %s\n", obj_dir_path( make, obj ), source->filename );
    output( "\t%s%s -o $@ %s\n", cmd_prefix( "MSG" ), msgfmt, source->filename );
    strarray_add( &make->all_targets, strmake( "%s.mo", obj ));
}


/*******************************************************************
 *         output_source_in
 */
static void output_source_in( struct makefile *make, struct incl_file *source, const char *obj )
{
    unsigned int i;

    if (strendswith( obj, ".man" ) && source->file->args)
    {
        struct strarray symlinks;
        char *dir, *dest = replace_extension( obj, ".man", "" );
        char *lang = strchr( dest, '.' );
        char *section = source->file->args;
        if (lang)
        {
            *lang++ = 0;
            dir = strmake( "$(mandir)/%s/man%s", lang, section );
        }
        else dir = strmake( "$(mandir)/man%s", section );
        add_install_rule( make, dest, xstrdup(obj), strmake( "d%s/%s.%s", dir, dest, section ));
        symlinks = get_expanded_file_local_var( make, dest, "SYMLINKS" );
        for (i = 0; i < symlinks.count; i++)
            add_install_rule( make, symlinks.str[i], strmake( "%s.%s", dest, section ),
                              strmake( "y%s/%s.%s", dir, symlinks.str[i], section ));
        free( dest );
        free( dir );
    }
    strarray_add( &make->in_files, xstrdup(obj) );
    strarray_add( &make->all_targets, xstrdup(obj) );
    output( "%s: %s\n", obj_dir_path( make, obj ), source->filename );
    output( "\t%s%s %s >$@ || (rm -f $@ && false)\n", cmd_prefix( "SED" ), sed_cmd, source->filename );
    output( "%s:", obj_dir_path( make, obj ));
    output_filenames( source->dependencies );
    output( "\n" );
    add_install_rule( make, obj, xstrdup( obj ), strmake( "d$(datadir)/wine/%s", obj ));
}


/*******************************************************************
 *         output_source_spec
 */
static void output_source_spec( struct makefile *make, struct incl_file *source, const char *obj )
{
    struct strarray imports = get_expanded_file_local_var( make, obj, "IMPORTS" );
    struct strarray dll_flags = get_expanded_file_local_var( make, obj, "EXTRADLLFLAGS" );
    struct strarray all_libs, dep_libs = empty_strarray;
    char *dll_name, *obj_name, *output_file;
    const char *debug_file;

    if (!imports.count) imports = make->imports;

    if (!dll_flags.count) dll_flags = make->extradllflags;
    all_libs = add_import_libs( make, &dep_libs, imports, 0, 0 );
    dll_name = strmake( "%s.dll%s", obj, make->is_cross ? "" : dll_ext );
    obj_name = strmake( "%s%s", obj_dir_path( make, obj ), make->is_cross ? ".cross.o" : ".o" );
    output_file = obj_dir_path( make, dll_name );

    strarray_add( &make->clean_files, dll_name );
    strarray_add( &make->res_files, strmake( "%s.res", obj ));
    output( "%s.res:", obj_dir_path( make, obj ));
    output_filename( obj_dir_path( make, dll_name ));
    output_filename( tools_path( make, "wrc" ));
    output( "\n" );
    output( "\t%secho \"%s.dll TESTDLL \\\"%s\\\"\" | %s -u -o $@\n", cmd_prefix( "WRC" ), obj, output_file,
            tools_path( make, "wrc" ));

    output( "%s:", output_file);
    output_filename( source->filename );
    output_filename( obj_name );
    output_filenames( dep_libs );
    output_filename( tools_path( make, "winebuild" ));
    output_filename( tools_path( make, "winegcc" ));
    output( "\n" );
    output_winegcc_command( make, make->is_cross );
    output_filename( "-s" );
    output_filenames( dll_flags );
    output_filename( "-shared" );
    output_filename( source->filename );
    output_filename( obj_name );
    if ((debug_file = get_debug_file( make, dll_name )))
        output_filename( strmake( "-Wl,--debug-file,%s", obj_dir_path( make, debug_file )));
    output_filenames( all_libs );
    output_filename( make->is_cross ? "$(CROSSLDFLAGS)" : "$(LDFLAGS)" );
    output( "\n" );
}


/*******************************************************************
 *         output_source_default
 */
static void output_source_default( struct makefile *make, struct incl_file *source, const char *obj )
{
    struct strarray defines = get_source_defines( make, source, obj );
    int is_dll_src = (make->testdll &&
                      strendswith( source->name, ".c" ) &&
                      find_src_file( make, replace_extension( source->name, ".c", ".spec" )));
    int need_cross = (crosstarget &&
                      !(source->file->flags & FLAG_C_UNIX) &&
                      (make->is_cross || make->staticlib ||
                       (source->file->flags & FLAG_C_IMPLIB)));
    int need_obj = ((*dll_ext || !(source->file->flags & FLAG_C_UNIX)) &&
                    (!need_cross ||
                     (source->file->flags & FLAG_C_IMPLIB) ||
                     (make->staticlib && !make->extlib)));

    if ((source->file->flags & FLAG_GENERATED) &&
        (!make->testdll || !strendswith( source->filename, "testlist.c" )))
        strarray_add( &make->clean_files, source->basename );
    if (source->file->flags & FLAG_C_IMPLIB) strarray_add( &make->implib_objs, strmake( "%s.o", obj ));

    if (need_obj)
    {
        if ((source->file->flags & FLAG_C_UNIX) && *dll_ext)
            strarray_add( &make->unixobj_files, strmake( "%s.o", obj ));
        else if (!is_dll_src && !(source->file->flags & FLAG_C_IMPLIB))
            strarray_add( &make->object_files, strmake( "%s.o", obj ));
        else
            strarray_add( &make->clean_files, strmake( "%s.o", obj ));
        output( "%s.o: %s\n", obj_dir_path( make, obj ), source->filename );
        output( "\t%s$(CC) -c -o $@ %s", cmd_prefix( "CC" ), source->filename );
        output_filenames( defines );
        if (make->sharedlib || (source->file->flags & FLAG_C_UNIX))
        {
            output_filenames( unix_dllflags );
        }
        else if (make->module || make->testdll)
        {
            output_filenames( dll_flags );
            if (source->use_msvcrt) output_filenames( msvcrt_flags );
            if (!*dll_ext && make->module && is_crt_module( make->module ))
                output_filename( "-fno-builtin" );
        }
        output_filenames( make->extlib ? extra_cflags_extlib : extra_cflags );
        output_filenames( cpp_flags );
        output_filename( "$(CFLAGS)" );
        output( "\n" );
    }
    if (need_cross)
    {
        if (!is_dll_src && !(source->file->flags & FLAG_C_IMPLIB))
            strarray_add( &make->crossobj_files, strmake( "%s.cross.o", obj ));
        else
            strarray_add( &make->clean_files, strmake( "%s.cross.o", obj ));
        output( "%s.cross.o: %s\n", obj_dir_path( make, obj ), source->filename );
        output( "\t%s$(CROSSCC) -c -o $@ %s", cmd_prefix( "CC" ), source->filename );
        output_filenames( defines );
        output_filenames( make->extlib ? extra_cross_cflags_extlib : extra_cross_cflags );
        if (make->module && is_crt_module( make->module ))
            output_filename( "-fno-builtin" );
        output_filenames( cpp_flags );
        output_filename( "$(CROSSCFLAGS)" );
        output( "\n" );
    }
    if (strendswith( source->name, ".c" ) && !(source->file->flags & FLAG_GENERATED))
    {
        strarray_add( &make->c2man_files, source->filename );
        if (make->testdll && !is_dll_src)
        {
            strarray_add( &make->ok_files, strmake( "%s.ok", obj ));
            output( "%s.ok:\n", obj_dir_path( make, obj ));
            output( "\t%s%s $(RUNTESTFLAGS) -T . -M %s -p %s%s %s && touch $@\n",
                    cmd_prefix( "TEST" ),
                    root_src_dir_path( "tools/runtest" ), make->testdll,
                    obj_dir_path( make, replace_extension( make->testdll, ".dll", "_test.exe" )),
                    make->is_cross ? "" : dll_ext, obj );
        }
    }
    if (need_obj) output_filename( strmake( "%s.o", obj_dir_path( make, obj )));
    if (need_cross) output_filename( strmake( "%s.cross.o", obj_dir_path( make, obj )));
    output( ":" );
    output_filenames( source->dependencies );
    output( "\n" );
}


/* dispatch table to output rules for a single source file */
static const struct
{
    const char *ext;
    void (*fn)( struct makefile *make, struct incl_file *source, const char *obj );
} output_source_funcs[] =
{
    { "y", output_source_y },
    { "l", output_source_l },
    { "h", output_source_h },
    { "rh", output_source_h },
    { "inl", output_source_h },
    { "rc", output_source_rc },
    { "mc", output_source_mc },
    { "res", output_source_res },
    { "idl", output_source_idl },
    { "sfd", output_source_sfd },
    { "svg", output_source_svg },
    { "nls", output_source_nls },
    { "desktop", output_source_desktop },
    { "po", output_source_po },
    { "in", output_source_in },
    { "x", output_source_x },
    { "spec", output_source_spec },
    { NULL, output_source_default }
};


/*******************************************************************
 *         get_unix_lib_name
 */
static char *get_unix_lib_name( struct makefile *make )
{
    struct incl_file *source;

    if (!*dll_ext) return NULL;
    LIST_FOR_EACH_ENTRY( source, &make->sources, struct incl_file, entry )
    {
        if (!(source->file->flags & FLAG_C_UNIX)) continue;
        return strmake( "%s%s", get_base_name( make->module ), dll_ext );
    }
    return NULL;
}


/*******************************************************************
 *         output_man_pages
 */
static void output_man_pages( struct makefile *make )
{
    if (make->c2man_files.count)
    {
        char *spec_file = src_dir_path( make, replace_extension( make->module, ".dll", ".spec" ));

        output( "manpages::\n" );
        output( "\t%s -w %s", root_src_dir_path( "tools/c2man.pl" ), spec_file );
        output_filename( strmake( "-R%s", root_src_dir_path( "" )));
        output_filename( strmake( "-I%s", root_src_dir_path( "include" )));
        output_filename( strmake( "-o documentation/man%s", man_ext ));
        output_filenames( make->c2man_files );
        output( "\n" );
        output( "htmlpages::\n" );
        output( "\t%s -Th -w %s", root_src_dir_path( "tools/c2man.pl" ), spec_file );
        output_filename( strmake( "-R%s", root_src_dir_path( "" )));
        output_filename( strmake( "-I%s", root_src_dir_path( "include" )));
        output_filename( "-o documentation/html" );
        output_filenames( make->c2man_files );
        output( "\n" );
        output( "sgmlpages::\n" );
        output( "\t%s -Ts -w %s", root_src_dir_path( "tools/c2man.pl" ), spec_file );
        output_filename( strmake( "-R%s", root_src_dir_path( "" )));
        output_filename( strmake( "-I%s", root_src_dir_path( "include" )));
        output_filename( "-o documentation/api-guide" );
        output_filenames( make->c2man_files );
        output( "\n" );
        output( "xmlpages::\n" );
        output( "\t%s -Tx -w %s", root_src_dir_path( "tools/c2man.pl" ), spec_file );
        output_filename( strmake( "-R%s", root_src_dir_path( "" )));
        output_filename( strmake( "-I%s", root_src_dir_path( "include" )));
        output_filename( "-o documentation/api-guide-xml" );
        output_filenames( make->c2man_files );
        output( "\n" );
        strarray_add( &make->phony_targets, "manpages" );
        strarray_add( &make->phony_targets, "htmlpages" );
        strarray_add( &make->phony_targets, "sgmlpages" );
        strarray_add( &make->phony_targets, "xmlpages" );
    }
}


/*******************************************************************
 *         output_module
 */
static void output_module( struct makefile *make )
{
    struct strarray all_libs = empty_strarray;
    struct strarray dep_libs = empty_strarray;
    char *module_path = obj_dir_path( make, make->module );
    const char *debug_file = NULL;
    char *spec_file = NULL;
    unsigned int i;

    if (!make->is_exe) spec_file = src_dir_path( make, replace_extension( make->module, ".dll", ".spec" ));
    strarray_addall( &all_libs, add_import_libs( make, &dep_libs, make->delayimports, 1, 0 ));
    strarray_addall( &all_libs, add_import_libs( make, &dep_libs, make->imports, 0, 0 ));

    if (make->is_cross)
    {
        if (delay_load_flag)
        {
            for (i = 0; i < make->delayimports.count; i++)
                strarray_add( &all_libs, strmake( "%s%s%s", delay_load_flag, make->delayimports.str[i],
                                                  strchr( make->delayimports.str[i], '.' ) ? "" : ".dll" ));
        }
        strarray_add( &make->all_targets, strmake( "%s", make->module ));
        add_install_rule( make, make->module, strmake( "%s", make->module ),
                          strmake( "c%s/%s", pe_dir, make->module ));
        debug_file = get_debug_file( make, make->module );
        output( "%s:", module_path );
    }
    else if (*dll_ext)
    {
        if (!make->use_msvcrt) strarray_addall( &all_libs, add_unix_libraries( make, &dep_libs ));
        for (i = 0; i < make->delayimports.count; i++)
            strarray_add( &all_libs, strmake( "-Wl,-delayload,%s%s", make->delayimports.str[i],
                                              strchr( make->delayimports.str[i], '.' ) ? "" : ".dll" ));
        strarray_add( &make->all_targets, strmake( "%s%s", make->module, dll_ext ));
        strarray_add( &make->all_targets, strmake( "%s.fake", make->module ));
        add_install_rule( make, make->module, strmake( "%s%s", make->module, dll_ext ),
                          strmake( "p%s/%s%s", so_dir, make->module, dll_ext ));
        add_install_rule( make, make->module, strmake( "%s.fake", make->module ),
                          strmake( "d%s/%s", pe_dir, make->module ));
        output( "%s%s %s.fake:", module_path, dll_ext, module_path );
    }
    else
    {
        strarray_addall( &all_libs, add_unix_libraries( make, &dep_libs ));
        strarray_add( &make->all_targets, make->module );
        add_install_rule( make, make->module, make->module,
                          strmake( "p$(%s)/%s", spec_file ? "dlldir" : "bindir", make->module ));
        debug_file = get_debug_file( make, make->module );
        output( "%s:", module_path );
    }

    if (spec_file) output_filename( spec_file );
    output_filenames_obj_dir( make, make->is_cross ? make->crossobj_files : make->object_files );
    output_filenames_obj_dir( make, make->res_files );
    output_filenames( dep_libs );
    output_filename( tools_path( make, "winebuild" ));
    output_filename( tools_path( make, "winegcc" ));
    output( "\n" );
    output_winegcc_command( make, make->is_cross );
    if (make->is_cross) output_filename( "-Wl,--wine-builtin" );
    if (spec_file)
    {
        output_filename( "-shared" );
        output_filename( spec_file );
    }
    output_filenames( make->extradllflags );
    output_filenames_obj_dir( make, make->is_cross ? make->crossobj_files : make->object_files );
    output_filenames_obj_dir( make, make->res_files );
    if (debug_file) output_filename( strmake( "-Wl,--debug-file,%s", obj_dir_path( make, debug_file )));
    output_filenames( all_libs );
    output_filename( make->is_cross ? "$(CROSSLDFLAGS)" : "$(LDFLAGS)" );
    output( "\n" );

    if (make->unixlib)
    {
        struct strarray unix_libs = empty_strarray;
        struct strarray unix_deps = empty_strarray;

        if (!make->native_unix_lib)
        {
            struct strarray unix_imports = empty_strarray;

            strarray_add( &unix_imports, "ntdll" );
            strarray_add( &unix_deps, obj_dir_path( top_makefile, "dlls/ntdll/ntdll.so" ));
            strarray_add( &unix_imports, "winecrt0" );
            if (spec_file) strarray_add( &unix_deps, spec_file );

            strarray_addall( &unix_libs, add_import_libs( make, &unix_deps, unix_imports, 0, 1 ));
        }

        strarray_addall( &unix_libs, add_unix_libraries( make, &unix_deps ));

        strarray_add( &make->all_targets, make->unixlib );
        add_install_rule( make, make->module, make->unixlib, strmake( "p%s/%s", so_dir, make->unixlib ));
        output( "%s:", obj_dir_path( make, make->unixlib ));
        output_filenames_obj_dir( make, make->unixobj_files );
        output_filenames( unix_deps );

        if (make->native_unix_lib)
        {
            output( "\n" );
            output( "\t%s$(CC) -o $@", cmd_prefix( "CCLD" ));
            output_filenames( get_expanded_make_var_array( make, "UNIXLDFLAGS" ));
        }
        else
        {
            output_filename( tools_path( make, "winebuild" ));
            output_filename( tools_path( make, "winegcc" ));
            output( "\n" );
            output_winegcc_command( make, 0 );
            output_filename( "-munix" );
            output_filename( "-shared" );
            if (spec_file) output_filename( spec_file );
        }
        output_filenames_obj_dir( make, make->unixobj_files );
        output_filenames( unix_libs );
        output_filename( "$(LDFLAGS)" );
        output( "\n" );
    }

    if (spec_file && make->importlib)
    {
        char *importlib_path = obj_dir_path( make, strmake( "lib%s", make->importlib ));

        strarray_add( &make->clean_files, strmake( "lib%s.a", make->importlib ));
        if (!*dll_ext && needs_delay_lib( make ))
        {
            strarray_add( &make->clean_files, strmake( "lib%s.delay.a", make->importlib ));
            output( "%s.delay.a ", importlib_path );
        }
        output( "%s.a: %s %s", importlib_path, tools_path( make, "winebuild" ), spec_file );
        output_filenames_obj_dir( make, make->implib_objs );
        output( "\n" );
        output( "\t%s%s -w --implib -o $@", cmd_prefix( "BUILD" ), tools_path( make, "winebuild" ) );
        output_filenames( target_flags );
        if (make->is_win16) output_filename( "-m16" );
        output_filename( "--export" );
        output_filename( spec_file );
        output_filenames_obj_dir( make, make->implib_objs );
        output( "\n" );
        add_install_rule( make, make->importlib,
                          strmake( "lib%s.a", make->importlib ),
                          strmake( "d%s/lib%s.a", so_dir, make->importlib ));

        if (crosstarget)
        {
            struct strarray cross_files = strarray_replace_extension( &make->implib_objs, ".o", ".cross.o" );

            strarray_add( &make->clean_files, strmake( "lib%s.cross.a", make->importlib ));
            output_filename( strmake( "%s.cross.a", importlib_path ));
            if (needs_delay_lib( make ))
            {
                strarray_add( &make->clean_files, strmake( "lib%s.delay.a", make->importlib ));
                output_filename( strmake( "%s.delay.a", importlib_path ));
            }
            output( ": %s %s", tools_path( make, "winebuild" ), spec_file );
            output_filenames_obj_dir( make, cross_files );
            output( "\n" );
            output( "\t%s%s -b %s -w --implib -o $@", cmd_prefix( "BUILD" ),
                    tools_path( make, "winebuild" ), crosstarget );
            if (make->is_win16) output_filename( "-m16" );
            output_filename( "--export" );
            output_filename( spec_file );
            output_filenames_obj_dir( make, cross_files );
            output( "\n" );
            add_install_rule( make, make->importlib,
                              strmake( "lib%s.cross.a", make->importlib ),
                              strmake( "d%s/lib%s.a", pe_dir, make->importlib ));
        }
        if (needs_implib_symlink( make ))
            strarray_addall( &top_makefile->clean_files, output_importlib_symlinks( make ));
    }

    if (spec_file)
        output_man_pages( make );
    else if (*dll_ext && !make->is_win16 && strendswith( make->module, ".exe" ))
    {
        char *binary = replace_extension( make->module, ".exe", "" );
        add_install_rule( make, binary, "wineapploader", strmake( "t$(bindir)/%s", binary ));
    }
}


/*******************************************************************
 *         output_static_lib
 */
static void output_static_lib( struct makefile *make )
{
    if (!crosstarget || !make->extlib)
    {
        strarray_add( &make->clean_files, make->staticlib );
        output( "%s: %s", obj_dir_path( make, make->staticlib ), tools_path( make, "winebuild" ));
        output_filenames_obj_dir( make, make->object_files );
        output_filenames_obj_dir( make, make->unixobj_files );
        output( "\n" );
        output( "\t%s%s -w --staticlib -o $@", cmd_prefix( "BUILD" ), tools_path( make, "winebuild" ));
        output_filenames( target_flags );
        output_filenames_obj_dir( make, make->object_files );
        output_filenames_obj_dir( make, make->unixobj_files );
        output( "\n" );
        add_install_rule( make, make->staticlib, make->staticlib,
                          strmake( "d%s/%s", so_dir, make->staticlib ));
    }
    if (crosstarget)
    {
        char *name = replace_extension( make->staticlib, ".a", ".cross.a" );

        strarray_add( &make->clean_files, name );
        output( "%s: %s", obj_dir_path( make, name ), tools_path( make, "winebuild" ));
        output_filenames_obj_dir( make, make->crossobj_files );
        output( "\n" );
        output( "\t%s%s -b %s -w --staticlib -o $@", cmd_prefix( "BUILD" ),
                tools_path( make, "winebuild" ), crosstarget );
        output_filenames_obj_dir( make, make->crossobj_files );
        output( "\n" );
        if (!make->extlib)
            add_install_rule( make, make->staticlib, name,
                              strmake( "d%s/%s", pe_dir, make->staticlib ));
    }
}


/*******************************************************************
 *         output_shared_lib
 */
static void output_shared_lib( struct makefile *make )
{
    unsigned int i;
    char *basename, *p;
    struct strarray names = get_shared_lib_names( make->sharedlib );
    struct strarray all_libs = empty_strarray;
    struct strarray dep_libs = empty_strarray;

    basename = xstrdup( make->sharedlib );
    if ((p = strchr( basename, '.' ))) *p = 0;

    strarray_addall( &dep_libs, get_local_dependencies( make, basename, make->in_files ));
    strarray_addall( &all_libs, get_expanded_file_local_var( make, basename, "LDFLAGS" ));
    strarray_addall( &all_libs, add_unix_libraries( make, &dep_libs ));

    output( "%s:", obj_dir_path( make, make->sharedlib ));
    output_filenames_obj_dir( make, make->object_files );
    output_filenames( dep_libs );
    output( "\n" );
    output( "\t%s$(CC) -o $@", cmd_prefix( "CCLD" ));
    output_filenames_obj_dir( make, make->object_files );
    output_filenames( all_libs );
    output_filename( "$(LDFLAGS)" );
    output( "\n" );
    add_install_rule( make, make->sharedlib, make->sharedlib, strmake( "p%s/%s", so_dir, make->sharedlib ));
    for (i = 1; i < names.count; i++)
    {
        output( "%s: %s\n", obj_dir_path( make, names.str[i] ), obj_dir_path( make, names.str[i-1] ));
        output_symlink_rule( names.str[i-1], obj_dir_path( make, names.str[i] ), 0 );
        add_install_rule( make, names.str[i], names.str[i-1], strmake( "y%s/%s", so_dir, names.str[i] ));
    }
    strarray_addall( &make->all_targets, names );
}


/*******************************************************************
 *         output_test_module
 */
static void output_test_module( struct makefile *make )
{
    char *testmodule = replace_extension( make->testdll, ".dll", "_test.exe" );
    char *stripped = replace_extension( make->testdll, ".dll", "_test-stripped.exe" );
    char *testres = replace_extension( make->testdll, ".dll", "_test.res" );
    struct strarray dep_libs = empty_strarray;
    struct strarray all_libs = add_import_libs( make, &dep_libs, make->imports, 0, 0 );
    struct makefile *parent = get_parent_makefile( make );
    const char *ext = make->is_cross ? "" : dll_ext;
    const char *debug_file;
    char *output_file;

    strarray_add( &make->all_targets, strmake( "%s%s", testmodule, ext ));
    strarray_add( &make->clean_files, strmake( "%s%s", stripped, ext ));
    output_file = strmake( "%s%s", obj_dir_path( make, testmodule ), ext );
    output( "%s:\n", output_file );
    output_winegcc_command( make, make->is_cross );
    output_filenames( make->extradllflags );
    output_filenames_obj_dir( make, make->is_cross ? make->crossobj_files : make->object_files );
    output_filenames_obj_dir( make, make->res_files );
    if ((debug_file = get_debug_file( make, testmodule )))
        output_filename( strmake( "-Wl,--debug-file,%s", obj_dir_path( make, debug_file )));
    output_filenames( all_libs );
    output_filename( make->is_cross ? "$(CROSSLDFLAGS)" : "$(LDFLAGS)" );
    output( "\n" );
    output( "%s%s:\n", obj_dir_path( make, stripped ), ext );
    output_winegcc_command( make, make->is_cross );
    output_filename( "-s" );
    output_filename( strmake( "-Wb,-F,%s", testmodule ));
    output_filenames( make->extradllflags );
    output_filenames_obj_dir( make, make->is_cross ? make->crossobj_files : make->object_files );
    output_filenames_obj_dir( make, make->res_files );
    output_filenames( all_libs );
    output_filename( make->is_cross ? "$(CROSSLDFLAGS)" : "$(LDFLAGS)" );
    output( "\n" );
    output( "%s%s %s%s:", obj_dir_path( make, testmodule ), ext, obj_dir_path( make, stripped ), ext );
    output_filenames_obj_dir( make, make->is_cross ? make->crossobj_files : make->object_files );
    output_filenames_obj_dir( make, make->res_files );
    output_filenames( dep_libs );
    output_filename( tools_path( make, "winebuild" ));
    output_filename( tools_path( make, "winegcc" ));
    output( "\n" );

    output( "programs/winetest/%s: %s%s\n", testres, obj_dir_path( make, stripped ), ext );
    output( "\t%secho \"%s TESTRES \\\"%s%s\\\"\" | %s -u -o $@\n", cmd_prefix( "WRC" ),
            testmodule, obj_dir_path( make, stripped ), ext, tools_path( make, "wrc" ));

    output_filenames_obj_dir( make, make->ok_files );
    output( ": %s%s", obj_dir_path( make, testmodule ), ext );
    if (parent)
    {
        output_filename( parent->is_cross ? obj_dir_path( parent, make->testdll )
                         : strmake( "%s%s", obj_dir_path( parent, make->testdll ), dll_ext ));
        if (parent->unixlib) output_filename( obj_dir_path( parent, parent->unixlib ));
    }
    output( "\n" );
    output( "%s %s:", obj_dir_path( make, "check" ), obj_dir_path( make, "test" ));
    if (!make->disabled && parent && !parent->disabled) output_filenames_obj_dir( make, make->ok_files );
    output( "\n" );
    strarray_add_uniq( &make->phony_targets, obj_dir_path( make, "check" ));
    strarray_add_uniq( &make->phony_targets, obj_dir_path( make, "test" ));
    output( "%s::\n", obj_dir_path( make, "testclean" ));
    output( "\trm -f" );
    output_filenames_obj_dir( make, make->ok_files );
    output( "\n" );
    strarray_addall( &make->clean_files, make->ok_files );
    strarray_add_uniq( &make->phony_targets, obj_dir_path( make, "testclean" ));
}


/*******************************************************************
 *         output_programs
 */
static void output_programs( struct makefile *make )
{
    unsigned int i, j;

    for (i = 0; i < make->programs.count; i++)
    {
        char *program_installed = NULL;
        char *program = strmake( "%s%s", make->programs.str[i], exe_ext );
        struct strarray deps = get_local_dependencies( make, make->programs.str[i], make->in_files );
        struct strarray all_libs = get_expanded_file_local_var( make, make->programs.str[i], "LDFLAGS" );
        struct strarray objs     = get_expanded_file_local_var( make, make->programs.str[i], "OBJS" );
        struct strarray symlinks = get_expanded_file_local_var( make, make->programs.str[i], "SYMLINKS" );

        if (!objs.count) objs = make->object_files;
        if (!strarray_exists( &all_libs, "-nodefaultlibs" ))
            strarray_addall( &all_libs, add_unix_libraries( make, &deps ));

        output( "%s:", obj_dir_path( make, program ) );
        output_filenames_obj_dir( make, objs );
        output_filenames( deps );
        output( "\n" );
        output( "\t%s$(CC) -o $@", cmd_prefix( "CC" ));
        output_filenames_obj_dir( make, objs );
        output_filenames( all_libs );
        output_filename( "$(LDFLAGS)" );
        output( "\n" );
        strarray_add( &make->all_targets, program );

        for (j = 0; j < symlinks.count; j++)
        {
            output( "%s: %s\n", obj_dir_path( make, symlinks.str[j] ), obj_dir_path( make, program ));
            output_symlink_rule( program, obj_dir_path( make, symlinks.str[j] ), 0 );
        }
        strarray_addall( &make->all_targets, symlinks );

        add_install_rule( make, program, program_installed ? program_installed : program,
                          strmake( "p$(bindir)/%s", program ));
        for (j = 0; j < symlinks.count; j++)
            add_install_rule( make, symlinks.str[j], program,
                              strmake( "y$(bindir)/%s%s", symlinks.str[j], exe_ext ));
    }
}


/*******************************************************************
 *         output_subdirs
 */
static void output_subdirs( struct makefile *make )
{
    struct strarray all_targets = empty_strarray;
    struct strarray makefile_deps = empty_strarray;
    struct strarray clean_files = empty_strarray;
    struct strarray testclean_files = empty_strarray;
    struct strarray distclean_files = empty_strarray;
    struct strarray dependencies = empty_strarray;
    struct strarray install_lib_deps = empty_strarray;
    struct strarray install_dev_deps = empty_strarray;
    struct strarray tooldeps_deps = empty_strarray;
    struct strarray buildtest_deps = empty_strarray;
    unsigned int i;

    strarray_addall( &clean_files, make->clean_files );
    strarray_addall( &distclean_files, make->distclean_files );
    strarray_addall( &all_targets, make->all_targets );
    for (i = 0; i < subdirs.count; i++)
    {
        strarray_add( &makefile_deps, src_dir_path( submakes[i], "Makefile.in" ));
        strarray_addall_uniq( &make->phony_targets, submakes[i]->phony_targets );
        strarray_addall_uniq( &make->uninstall_files, submakes[i]->uninstall_files );
        strarray_addall_uniq( &dependencies, submakes[i]->dependencies );
        strarray_addall_path( &clean_files, submakes[i]->obj_dir, submakes[i]->clean_files );
        strarray_addall_path( &distclean_files, submakes[i]->obj_dir, submakes[i]->distclean_files );
        strarray_addall_path( &testclean_files, submakes[i]->obj_dir, submakes[i]->ok_files );
        strarray_addall_path( &make->pot_files, submakes[i]->obj_dir, submakes[i]->pot_files );

        if (submakes[i]->disabled) continue;

        strarray_addall_path( &all_targets, submakes[i]->obj_dir, submakes[i]->all_targets );
        if (!strcmp( submakes[i]->obj_dir, "tools" ) || !strncmp( submakes[i]->obj_dir, "tools/", 6 ))
            strarray_add( &tooldeps_deps, obj_dir_path( submakes[i], "all" ));
        if (submakes[i]->testdll)
            strarray_add( &buildtest_deps, obj_dir_path( submakes[i], "all" ));
        if (submakes[i]->install_rules[INSTALL_LIB].count)
            strarray_add( &install_lib_deps, obj_dir_path( submakes[i], "install-lib" ));
        if (submakes[i]->install_rules[INSTALL_DEV].count)
            strarray_add( &install_dev_deps, obj_dir_path( submakes[i], "install-dev" ));
    }
    strarray_addall( &dependencies, makefile_deps );
    output( "all:" );
    output_filenames( all_targets );
    output( "\n" );
    output( "Makefile:" );
    output_filenames( makefile_deps );
    output( "\n" );
    output_filenames( dependencies );
    output( ":\n" );
    if (install_lib_deps.count)
    {
        output( "install install-lib::" );
        output_filenames( install_lib_deps );
        output( "\n" );
        strarray_add_uniq( &make->phony_targets, "install" );
        strarray_add_uniq( &make->phony_targets, "install-lib" );
    }
    if (install_dev_deps.count)
    {
        output( "install install-dev::" );
        output_filenames( install_dev_deps );
        output( "\n" );
        strarray_add_uniq( &make->phony_targets, "install" );
        strarray_add_uniq( &make->phony_targets, "install-dev" );
    }
    output_uninstall_rules( make );
    if (buildtest_deps.count)
    {
        output( "buildtests:" );
        output_filenames( buildtest_deps );
        output( "\n" );
        strarray_add_uniq( &make->phony_targets, "buildtests" );
    }
    output( "check test:" );
    output_filenames( testclean_files );
    output( "\n" );
    strarray_add_uniq( &make->phony_targets, "check" );
    strarray_add_uniq( &make->phony_targets, "test" );

    output( "clean::\n");
    output_rm_filenames( clean_files );
    output( "testclean::\n");
    output_rm_filenames( testclean_files );
    output( "distclean::\n");
    output_rm_filenames( distclean_files );
    strarray_add_uniq( &make->phony_targets, "distclean" );
    strarray_add_uniq( &make->phony_targets, "testclean" );

    if (tooldeps_deps.count)
    {
        output( "__tooldeps__:" );
        output_filenames( tooldeps_deps );
        output( "\n" );
        strarray_add_uniq( &make->phony_targets, "__tooldeps__" );
    }

    if (get_expanded_make_variable( make, "GETTEXTPO_LIBS" )) output_po_files( make );

    if (make->phony_targets.count)
    {
        output( ".PHONY:" );
        output_filenames( make->phony_targets );
        output( "\n" );
    }
}


/*******************************************************************
 *         output_sources
 */
static void output_sources( struct makefile *make )
{
    struct strarray all_targets = empty_strarray;
    struct incl_file *source;
    unsigned int i, j;

    strarray_add_uniq( &make->phony_targets, "all" );

    LIST_FOR_EACH_ENTRY( source, &make->sources, struct incl_file, entry )
    {
        char *obj = xstrdup( source->name );
        char *ext = get_extension( obj );

        if (!ext) fatal_error( "unsupported file type %s\n", source->name );
        *ext++ = 0;

        for (j = 0; output_source_funcs[j].ext; j++)
            if (!strcmp( ext, output_source_funcs[j].ext )) break;

        output_source_funcs[j].fn( make, source, obj );
        strarray_addall_uniq( &make->dependencies, source->dependencies );
    }

    /* special case for winetest: add resource files from other test dirs */
    if (make->obj_dir && !strcmp( make->obj_dir, "programs/winetest" ))
    {
        struct strarray tests = enable_tests;
        if (!tests.count)
            for (i = 0; i < subdirs.count; i++)
                if (submakes[i]->testdll && !submakes[i]->disabled)
                    strarray_add( &tests, submakes[i]->testdll );
        for (i = 0; i < tests.count; i++)
            strarray_add( &make->res_files, replace_extension( tests.str[i], ".dll", "_test.res" ));
    }

    if (make->dlldata_files.count)
    {
        output( "%s: %s %s\n", obj_dir_path( make, "dlldata.c" ),
                tools_path( make, "widl" ), src_dir_path( make, "Makefile.in" ));
        output( "\t%s%s --dlldata-only -o $@", cmd_prefix( "WIDL" ), tools_path( make, "widl" ));
        output_filenames( make->dlldata_files );
        output( "\n" );
    }

    if (make->staticlib) output_static_lib( make );
    else if (make->module) output_module( make );
    else if (make->testdll) output_test_module( make );
    else if (make->sharedlib) output_shared_lib( make );
    else if (make->programs.count) output_programs( make );

    for (i = 0; i < make->scripts.count; i++)
        add_install_rule( make, make->scripts.str[i], make->scripts.str[i],
                          strmake( "S$(bindir)/%s", make->scripts.str[i] ));

    for (i = 0; i < make->extra_targets.count; i++)
        if (strarray_exists( &make->dependencies, obj_dir_path( make, make->extra_targets.str[i] )))
            strarray_add( &make->clean_files, make->extra_targets.str[i] );
        else
            strarray_add( &make->all_targets, make->extra_targets.str[i] );

    if (!make->src_dir) strarray_add( &make->distclean_files, ".gitignore" );
    strarray_add( &make->distclean_files, "Makefile" );
    if (make->testdll) strarray_add( &make->distclean_files, "testlist.c" );

    if (!make->obj_dir)
        strarray_addall( &make->distclean_files, get_expanded_make_var_array( make, "CONFIGURE_TARGETS" ));
    else if (!strcmp( make->obj_dir, "po" ))
        strarray_add( &make->distclean_files, "LINGUAS" );

    strarray_addall( &make->clean_files, make->object_files );
    strarray_addall( &make->clean_files, make->crossobj_files );
    strarray_addall( &make->clean_files, make->unixobj_files );
    strarray_addall( &make->clean_files, make->res_files );
    strarray_addall( &make->clean_files, make->pot_files );
    strarray_addall( &make->clean_files, make->debug_files );
    strarray_addall( &make->clean_files, make->all_targets );

    if (make == top_makefile)
    {
        output_subdirs( make );
        return;
    }

    strarray_addall( &all_targets, make->all_targets );
    strarray_addall( &all_targets, make->font_files );
    if (all_targets.count)
    {
        output( "%s:", obj_dir_path( make, "all" ));
        output_filenames_obj_dir( make, all_targets );
        output( "\n" );
        strarray_add_uniq( &make->phony_targets, obj_dir_path( make, "all" ));
    }
    output_install_rules( make, INSTALL_LIB, "install-lib" );
    output_install_rules( make, INSTALL_DEV, "install-dev" );

    if (make->clean_files.count)
    {
        output( "%s::\n", obj_dir_path( make, "clean" ));
        output( "\trm -f" );
        output_filenames_obj_dir( make, make->clean_files );
        output( "\n" );
        strarray_add( &make->phony_targets, obj_dir_path( make, "clean" ));
    }
}


/*******************************************************************
 *         create_temp_file
 */
static FILE *create_temp_file( const char *orig )
{
    char *name = xmalloc( strlen(orig) + 13 );
    unsigned int i, id = getpid();
    int fd;
    FILE *ret = NULL;

    for (i = 0; i < 100; i++)
    {
        sprintf( name, "%s.tmp%08x", orig, id );
        if ((fd = open( name, O_RDWR | O_CREAT | O_EXCL, 0666 )) != -1)
        {
            ret = fdopen( fd, "w" );
            break;
        }
        if (errno != EEXIST) break;
        id += 7777;
    }
    if (!ret) fatal_error( "failed to create output file for '%s'\n", orig );
    temp_file_name = name;
    return ret;
}


/*******************************************************************
 *         rename_temp_file
 */
static void rename_temp_file( const char *dest )
{
    int ret = rename( temp_file_name, dest );
    if (ret == -1 && errno == EEXIST)
    {
        /* rename doesn't overwrite on windows */
        unlink( dest );
        ret = rename( temp_file_name, dest );
    }
    if (ret == -1) fatal_error( "failed to rename output file to '%s'\n", dest );
    temp_file_name = NULL;
}


/*******************************************************************
 *         are_files_identical
 */
static int are_files_identical( FILE *file1, FILE *file2 )
{
    for (;;)
    {
        char buffer1[8192], buffer2[8192];
        int size1 = fread( buffer1, 1, sizeof(buffer1), file1 );
        int size2 = fread( buffer2, 1, sizeof(buffer2), file2 );
        if (size1 != size2) return 0;
        if (!size1) return feof( file1 ) && feof( file2 );
        if (memcmp( buffer1, buffer2, size1 )) return 0;
    }
}


/*******************************************************************
 *         rename_temp_file_if_changed
 */
static void rename_temp_file_if_changed( const char *dest )
{
    FILE *file1, *file2;
    int do_rename = 1;

    if ((file1 = fopen( dest, "r" )))
    {
        if ((file2 = fopen( temp_file_name, "r" )))
        {
            do_rename = !are_files_identical( file1, file2 );
            fclose( file2 );
        }
        fclose( file1 );
    }
    if (!do_rename)
    {
        unlink( temp_file_name );
        temp_file_name = NULL;
    }
    else rename_temp_file( dest );
}


/*******************************************************************
 *         output_linguas
 */
static void output_linguas( const struct makefile *make )
{
    const char *dest = obj_dir_path( make, "LINGUAS" );
    struct incl_file *source;

    output_file = create_temp_file( dest );

    output( "# Automatically generated by make depend; DO NOT EDIT!!\n" );
    LIST_FOR_EACH_ENTRY( source, &make->sources, struct incl_file, entry )
        if (strendswith( source->name, ".po" ))
            output( "%s\n", replace_extension( source->name, ".po", "" ));

    if (fclose( output_file )) fatal_perror( "write" );
    output_file = NULL;
    rename_temp_file_if_changed( dest );
}


/*******************************************************************
 *         output_testlist
 */
static void output_testlist( const struct makefile *make )
{
    const char *dest = obj_dir_path( make, "testlist.c" );
    struct strarray files = empty_strarray;
    unsigned int i;

    for (i = 0; i < make->ok_files.count; i++)
        strarray_add( &files, replace_extension( make->ok_files.str[i], ".ok", "" ));

    output_file = create_temp_file( dest );

    output( "/* Automatically generated by make depend; DO NOT EDIT!! */\n\n" );
    output( "#define WIN32_LEAN_AND_MEAN\n" );
    output( "#include <windows.h>\n\n" );
    output( "#define STANDALONE\n" );
    output( "#include \"wine/test.h\"\n\n" );

    for (i = 0; i < files.count; i++) output( "extern void func_%s(void);\n", files.str[i] );
    output( "\n" );
    output( "const struct test winetest_testlist[] =\n" );
    output( "{\n" );
    for (i = 0; i < files.count; i++) output( "    { \"%s\", func_%s },\n", files.str[i], files.str[i] );
    output( "    { 0, 0 }\n" );
    output( "};\n" );

    if (fclose( output_file )) fatal_perror( "write" );
    output_file = NULL;
    rename_temp_file_if_changed( dest );
}


/*******************************************************************
 *         output_gitignore
 */
static void output_gitignore( const char *dest, struct strarray files )
{
    int i;

    output_file = create_temp_file( dest );

    output( "# Automatically generated by make depend; DO NOT EDIT!!\n" );
    for (i = 0; i < files.count; i++)
    {
        if (!strchr( files.str[i], '/' )) output( "/" );
        output( "%s\n", files.str[i] );
    }

    if (fclose( output_file )) fatal_perror( "write" );
    output_file = NULL;
    rename_temp_file( dest );
}


/*******************************************************************
 *         output_stub_makefile
 */
static void output_stub_makefile( struct makefile *make )
{
    struct strarray targets = empty_strarray;
    const char *make_var = strarray_get_value( &top_makefile->vars, "MAKE" );

    if (make->obj_dir) create_dir( make->obj_dir );

    output_file_name = obj_dir_path( make, "Makefile" );
    output_file = create_temp_file( output_file_name );

    output( "# Auto-generated stub makefile; all rules forward to the top-level makefile\n\n" );

    if (make_var) output( "MAKE = %s\n\n", make_var );
    output( "all:\n" );

    if (make->all_targets.count) strarray_add( &targets, "all" );
    if (make->install_rules[INSTALL_LIB].count || make->install_rules[INSTALL_DEV].count)
        strarray_add( &targets, "install" );
    if (make->install_rules[INSTALL_LIB].count) strarray_add( &targets, "install-lib" );
    if (make->install_rules[INSTALL_DEV].count) strarray_add( &targets, "install-dev" );
    if (make->clean_files.count) strarray_add( &targets, "clean" );
    if (make->ok_files.count)
    {
        strarray_add( &targets, "check" );
        strarray_add( &targets, "test" );
        strarray_add( &targets, "testclean" );
    }

    output_filenames( targets );
    output_filenames( make->clean_files );
    output( ":\n" );
    output( "\t@cd %s && $(MAKE) %s/$@\n", get_relative_path( make->obj_dir, "" ), make->obj_dir );
    output( ".PHONY:" );
    output_filenames( targets );
    output( "\n" );

    fclose( output_file );
    output_file = NULL;
    rename_temp_file( output_file_name );
}


/*******************************************************************
 *         output_silent_rules
 */
static void output_silent_rules(void)
{
    static const char *cmds[] =
    {
        "BISON",
        "BUILD",
        "CC",
        "CCLD",
        "FLEX",
        "GEN",
        "LN",
        "MSG",
        "SED",
        "TEST",
        "WIDL",
        "WMC",
        "WRC"
    };
    unsigned int i;

    output( "V = 0\n" );
    for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
    {
        output( "quiet_%s = $(quiet_%s_$(V))\n", cmds[i], cmds[i] );
        output( "quiet_%s_0 = @echo \"  %-5s \" $@;\n", cmds[i], cmds[i] );
        output( "quiet_%s_1 =\n", cmds[i] );
    }
}


/*******************************************************************
 *         output_top_makefile
 */
static void output_top_makefile( struct makefile *make )
{
    char buffer[1024];
    FILE *src_file;
    int i, found = 0;

    output_file_name = obj_dir_path( make, output_makefile_name );
    output_file = create_temp_file( output_file_name );

    /* copy the contents of the source makefile */
    src_file = open_input_makefile( make );
    while (fgets( buffer, sizeof(buffer), src_file ) && !found)
    {
        if (fwrite( buffer, 1, strlen(buffer), output_file ) != strlen(buffer)) fatal_perror( "write" );
        found = !strncmp( buffer, separator, strlen(separator) );
    }
    if (fclose( src_file )) fatal_perror( "close" );
    input_file_name = NULL;

    if (!found) output( "\n%s (everything below this line is auto-generated; DO NOT EDIT!!)\n", separator );

    if (silent_rules) output_silent_rules();
    for (i = 0; i < subdirs.count; i++) output_sources( submakes[i] );
    output_sources( make );
    /* disable implicit rules */
    output( ".SUFFIXES:\n" );

    fclose( output_file );
    output_file = NULL;
    rename_temp_file( output_file_name );
}


/*******************************************************************
 *         output_dependencies
 */
static void output_dependencies( struct makefile *make )
{
    struct strarray ignore_files = empty_strarray;

    if (make->obj_dir) create_dir( make->obj_dir );

    if (make == top_makefile) output_top_makefile( make );
    else output_stub_makefile( make );

    strarray_addall( &ignore_files, make->distclean_files );
    strarray_addall( &ignore_files, make->clean_files );
    if (make->testdll) output_testlist( make );
    if (make->obj_dir && !strcmp( make->obj_dir, "po" )) output_linguas( make );
    if (!make->src_dir) output_gitignore( obj_dir_path( make, ".gitignore" ), ignore_files );

    create_file_directories( make, ignore_files );

    output_file_name = NULL;
}


/*******************************************************************
 *         load_sources
 */
static void load_sources( struct makefile *make )
{
    static const char *source_vars[] =
    {
        "SOURCES",
        "C_SRCS",
        "OBJC_SRCS",
        "RC_SRCS",
        "MC_SRCS",
        "IDL_SRCS",
        "BISON_SRCS",
        "LEX_SRCS",
        "HEADER_SRCS",
        "XTEMPLATE_SRCS",
        "SVG_SRCS",
        "FONT_SRCS",
        "IN_SRCS",
        "PO_SRCS",
        "MANPAGES",
        NULL
    };
    const char **var;
    unsigned int i;
    struct strarray value;
    struct incl_file *file;

    strarray_set_value( &make->vars, "top_srcdir", root_src_dir_path( "" ));
    strarray_set_value( &make->vars, "srcdir", src_dir_path( make, "" ));

    make->parent_dir    = get_expanded_make_variable( make, "PARENTSRC" );
    make->module        = get_expanded_make_variable( make, "MODULE" );
    make->testdll       = get_expanded_make_variable( make, "TESTDLL" );
    make->sharedlib     = get_expanded_make_variable( make, "SHAREDLIB" );
    make->staticlib     = get_expanded_make_variable( make, "STATICLIB" );
    make->importlib     = get_expanded_make_variable( make, "IMPORTLIB" );
    make->extlib        = get_expanded_make_variable( make, "EXTLIB" );
    if (*dll_ext) make->unixlib = get_expanded_make_variable( make, "UNIXLIB" );

    make->programs      = get_expanded_make_var_array( make, "PROGRAMS" );
    make->scripts       = get_expanded_make_var_array( make, "SCRIPTS" );
    make->imports       = get_expanded_make_var_array( make, "IMPORTS" );
    make->delayimports  = get_expanded_make_var_array( make, "DELAYIMPORTS" );
    make->extradllflags = get_expanded_make_var_array( make, "EXTRADLLFLAGS" );
    make->install_lib   = get_expanded_make_var_array( make, "INSTALL_LIB" );
    make->install_dev   = get_expanded_make_var_array( make, "INSTALL_DEV" );
    make->extra_targets = get_expanded_make_var_array( make, "EXTRA_TARGETS" );

    if (make->extlib) make->staticlib = make->extlib;
    if (make->staticlib) make->module = make->staticlib;

    make->disabled   = make->obj_dir && strarray_exists( &disabled_dirs, make->obj_dir );
    make->is_win16   = strarray_exists( &make->extradllflags, "-m16" );
    make->use_msvcrt = (make->module || make->testdll || make->is_win16) &&
                       !strarray_exists( &make->extradllflags, "-mcygwin" );
    make->is_exe     = strarray_exists( &make->extradllflags, "-mconsole" ) ||
                       strarray_exists( &make->extradllflags, "-mwindows" );
    make->native_unix_lib = !!make->unixlib;

    if (make->use_msvcrt) strarray_add_uniq( &make->extradllflags, "-mno-cygwin" );

    if (make->module && !make->install_lib.count && !make->install_dev.count)
    {
        if (make->importlib) strarray_add( &make->install_dev, make->importlib );
        if (make->staticlib) strarray_add( &make->install_dev, make->staticlib );
        else strarray_add( &make->install_lib, make->module );
    }

    make->include_paths = empty_strarray;
    make->include_args = empty_strarray;
    make->define_args = empty_strarray;
    if (!make->extlib) strarray_add( &make->define_args, "-D__WINESRC__" );

    value = get_expanded_make_var_array( make, "EXTRAINCL" );
    for (i = 0; i < value.count; i++)
        if (!strncmp( value.str[i], "-I", 2 ))
            strarray_add_uniq( &make->include_paths, value.str[i] + 2 );
        else if (!strncmp( value.str[i], "-D", 2 ) || !strncmp( value.str[i], "-U", 2 ))
            strarray_add_uniq( &make->define_args, value.str[i] );
    strarray_addall( &make->define_args, get_expanded_make_var_array( make, "EXTRADEFS" ));

    strarray_add( &make->include_args, strmake( "-I%s", obj_dir_path( make, "" )));
    if (make->src_dir)
        strarray_add( &make->include_args, strmake( "-I%s", make->src_dir ));
    if (make->parent_dir)
        strarray_add( &make->include_args, strmake( "-I%s", src_dir_path( make, make->parent_dir )));
    strarray_add( &make->include_args, "-Iinclude" );
    if (root_src_dir) strarray_add( &make->include_args, strmake( "-I%s", root_src_dir_path( "include" )));

    list_init( &make->sources );
    list_init( &make->includes );

    for (var = source_vars; *var; var++)
    {
        value = get_expanded_make_var_array( make, *var );
        for (i = 0; i < value.count; i++) add_src_file( make, value.str[i] );
    }

    add_generated_sources( make );
    if (!make->unixlib) make->unixlib = get_unix_lib_name( make );

    if (make->use_msvcrt) strarray_add( &make->define_args, get_crt_define( make ));

    LIST_FOR_EACH_ENTRY( file, &make->includes, struct incl_file, entry ) parse_file( make, file, 0 );
    LIST_FOR_EACH_ENTRY( file, &make->sources, struct incl_file, entry ) get_dependencies( file, file );

    make->is_cross = crosstarget && make->use_msvcrt;

    if (!*dll_ext || make->is_cross)
        for (i = 0; i < make->delayimports.count; i++)
            strarray_add_uniq( &delay_import_libs, get_base_name( make->delayimports.str[i] ));
}


/*******************************************************************
 *         parse_makeflags
 */
static void parse_makeflags( const char *flags )
{
    const char *p = flags;
    char *var, *buffer = xmalloc( strlen(flags) + 1 );

    while (*p)
    {
        while (isspace(*p)) p++;
        var = buffer;
        while (*p && !isspace(*p))
        {
            if (*p == '\\' && p[1]) p++;
            *var++ = *p++;
        }
        *var = 0;
        if (var > buffer) set_make_variable( &cmdline_vars, buffer );
    }
}


/*******************************************************************
 *         parse_option
 */
static int parse_option( const char *opt )
{
    if (opt[0] != '-')
    {
        if (strchr( opt, '=' )) return set_make_variable( &cmdline_vars, opt );
        return 0;
    }
    switch(opt[1])
    {
    case 'f':
        if (opt[2]) output_makefile_name = opt + 2;
        break;
    case 'R':
        relative_dir_mode = 1;
        break;
    case 'S':
        silent_rules = 1;
        break;
    default:
        fprintf( stderr, "Unknown option '%s'\n%s", opt, Usage );
        exit(1);
    }
    return 1;
}


/*******************************************************************
 *         main
 */
int main( int argc, char *argv[] )
{
    const char *makeflags = getenv( "MAKEFLAGS" );
    int i, j;

    if (makeflags) parse_makeflags( makeflags );

    i = 1;
    while (i < argc)
    {
        if (parse_option( argv[i] ))
        {
            for (j = i; j < argc; j++) argv[j] = argv[j+1];
            argc--;
        }
        else i++;
    }

    if (relative_dir_mode)
    {
        char *relpath;

        if (argc != 3)
        {
            fprintf( stderr, "Option -R needs two directories\n%s", Usage );
            exit( 1 );
        }
        relpath = get_relative_path( argv[1], argv[2] );
        printf( "%s\n", relpath ? relpath : "." );
        exit( 0 );
    }

    if (argc > 1) fatal_error( "Directory arguments not supported in this mode\n" );

    atexit( cleanup_files );
    signal( SIGTERM, exit_on_signal );
    signal( SIGINT, exit_on_signal );
#ifdef SIGHUP
    signal( SIGHUP, exit_on_signal );
#endif

    for (i = 0; i < HASH_SIZE; i++) list_init( &files[i] );

    top_makefile = parse_makefile( NULL );

    target_flags       = get_expanded_make_var_array( top_makefile, "TARGETFLAGS" );
    msvcrt_flags       = get_expanded_make_var_array( top_makefile, "MSVCRTFLAGS" );
    dll_flags          = get_expanded_make_var_array( top_makefile, "DLLFLAGS" );
    extra_cflags       = get_expanded_make_var_array( top_makefile, "EXTRACFLAGS" );
    extra_cross_cflags = get_expanded_make_var_array( top_makefile, "EXTRACROSSCFLAGS" );
    unix_dllflags      = get_expanded_make_var_array( top_makefile, "UNIXDLLFLAGS" );
    cpp_flags          = get_expanded_make_var_array( top_makefile, "CPPFLAGS" );
    lddll_flags        = get_expanded_make_var_array( top_makefile, "LDDLLFLAGS" );
    libs               = get_expanded_make_var_array( top_makefile, "LIBS" );
    enable_tests       = get_expanded_make_var_array( top_makefile, "ENABLE_TESTS" );
    top_install_lib    = get_expanded_make_var_array( top_makefile, "TOP_INSTALL_LIB" );
    top_install_dev    = get_expanded_make_var_array( top_makefile, "TOP_INSTALL_DEV" );

    delay_load_flag    = get_expanded_make_variable( top_makefile, "DELAYLOADFLAG" );
    root_src_dir       = get_expanded_make_variable( top_makefile, "srcdir" );
    tools_dir          = get_expanded_make_variable( top_makefile, "toolsdir" );
    tools_ext          = get_expanded_make_variable( top_makefile, "toolsext" );
    exe_ext            = get_expanded_make_variable( top_makefile, "EXEEXT" );
    man_ext            = get_expanded_make_variable( top_makefile, "api_manext" );
    dll_ext            = (exe_ext && !strcmp( exe_ext, ".exe" )) ? "" : ".so";
    host_cpu           = get_expanded_make_variable( top_makefile, "host_cpu" );
    crosstarget        = get_expanded_make_variable( top_makefile, "CROSSTARGET" );
    crossdebug         = get_expanded_make_variable( top_makefile, "CROSSDEBUG" );
    fontforge          = get_expanded_make_variable( top_makefile, "FONTFORGE" );
    convert            = get_expanded_make_variable( top_makefile, "CONVERT" );
    flex               = get_expanded_make_variable( top_makefile, "FLEX" );
    bison              = get_expanded_make_variable( top_makefile, "BISON" );
    ar                 = get_expanded_make_variable( top_makefile, "AR" );
    ranlib             = get_expanded_make_variable( top_makefile, "RANLIB" );
    rsvg               = get_expanded_make_variable( top_makefile, "RSVG" );
    icotool            = get_expanded_make_variable( top_makefile, "ICOTOOL" );
    dlltool            = get_expanded_make_variable( top_makefile, "DLLTOOL" );
    msgfmt             = get_expanded_make_variable( top_makefile, "MSGFMT" );
    sed_cmd            = get_expanded_make_variable( top_makefile, "SED_CMD" );
    ln_s               = get_expanded_make_variable( top_makefile, "LN_S" );

    if (root_src_dir && !strcmp( root_src_dir, "." )) root_src_dir = NULL;
    if (tools_dir && !strcmp( tools_dir, "." )) tools_dir = NULL;
    if (!exe_ext) exe_ext = "";
    if (!tools_ext) tools_ext = "";
    if (!man_ext) man_ext = "3w";
    if (host_cpu && (host_cpu = normalize_arch( host_cpu )))
    {
        so_dir = strmake( "$(dlldir)/%s-unix", host_cpu );
        pe_dir = strmake( "$(dlldir)/%s-windows", host_cpu );
    }
    else
        so_dir = pe_dir = "$(dlldir)";

    extra_cflags_extlib = remove_warning_flags( extra_cflags );
    extra_cross_cflags_extlib = remove_warning_flags( extra_cross_cflags );

    top_makefile->src_dir = root_src_dir;
    subdirs = get_expanded_make_var_array( top_makefile, "SUBDIRS" );
    disabled_dirs = get_expanded_make_var_array( top_makefile, "DISABLED_SUBDIRS" );
    submakes = xmalloc( subdirs.count * sizeof(*submakes) );

    for (i = 0; i < subdirs.count; i++) submakes[i] = parse_makefile( subdirs.str[i] );

    load_sources( top_makefile );
    for (i = 0; i < subdirs.count; i++) load_sources( submakes[i] );

    output_dependencies( top_makefile );
    for (i = 0; i < subdirs.count; i++) output_dependencies( submakes[i] );

    return 0;
}
