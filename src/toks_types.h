/**
 * @file toks_types.h
 *
 * Defines some types for the toks program
 *
 * @author  Ben Gardner
 * @license GPL v2+
 */
#ifndef TOKS_TYPES_H_INCLUDED
#define TOKS_TYPES_H_INCLUDED

#include <vector>
#include <deque>
#include <cstdio>
#include <string>
using namespace std;

#include "base_types.h"
#include "token_enum.h"    /* c_token_t */
#include "log_levels.h"
#include "logger.h"
#include "sqlite3080200.h"
#include "ListManager.h"

/**
 * Brace stage enum used in brace_cleanup
 */
enum brstage_e
{
   BS_NONE,
   BS_PAREN1,    /* if/for/switch/while */
   BS_OP_PAREN1, /* optional paren: catch () { */
   BS_WOD_PAREN, /* while of do parens */
   BS_WOD_SEMI,  /* semicolon after while of do */
   BS_BRACE_DO,  /* do */
   BS_BRACE2,    /* if/else/for/switch/while */
   BS_ELSE,      /* expecting 'else' after 'if' */
   BS_ELSEIF,    /* expecting 'if' after 'else' */
   BS_WHILE,     /* expecting 'while' after 'do' */
   BS_CATCH,     /* expecting 'catch' or 'finally' after 'try' */
};

enum CharEncoding
{
   ENC_UTF8,
   ENC_UTF16_LE,
   ENC_UTF16_BE,
};

struct chunk_t;


/**
 * Structure for counting nested level
 */
struct paren_stack_entry
{
   c_token_t    type;         /**< the type that opened the entry */
   int          level;        /**< Level of opening type */
   int          open_line;    /**< line that open symbol is on */
   chunk_t      *pc;          /**< Chunk that opened the level */
   int          brace_indent; /**< indent for braces - may not relate to indent */
   int          indent;       /**< indent level (depends on use) */
   int          indent_tmp;   /**< temporary indent level (depends on use) */
   int          indent_tab;   /**< the 'tab' indent (always <= real column) */
   bool         indent_cont;  /**< indent_continue was applied */
   int          ref;
   c_token_t    parent;       /**< if, for, function, etc */
   brstage_e    stage;
   bool         in_preproc;   /**< whether this was created in a preprocessor */
   bool         non_vardef;   /**< Hit a non-vardef line */
};

/* TODO: put this on a linked list */
struct parse_frame
{
   int                      ref_no;
   int                      level;           // level of parens/square/angle/brace
   int                      brace_level;     // level of brace/vbrace
   int                      pp_level;        // level of preproc #if stuff

   int                      sparen_count;

   struct paren_stack_entry pse[128];
   int                      pse_tos;
   int                      paren_count;

   c_token_t                in_ifdef;
   int                      stmt_count;
   int                      expr_count;

   bool                     maybe_decl;
   bool                     maybe_cast;
};

#define PCF_BIT(b)   (1ULL << b)

/* Copy flags are in the lower 16 bits */
#define PCF_COPY_FLAGS         0x0000ffff
#define PCF_IN_PREPROC         PCF_BIT(0)  /* in a preprocessor */
#define PCF_IN_STRUCT          PCF_BIT(1)  /* in a struct */
#define PCF_IN_ENUM            PCF_BIT(2)  /* in enum */
#define PCF_IN_FCN_DEF         PCF_BIT(3)  /* inside function def parens */
#define PCF_IN_FCN_CALL        PCF_BIT(4)  /* inside function call parens */
#define PCF_IN_SPAREN          PCF_BIT(5)  /* inside for/if/while/switch parens */
#define PCF_IN_TEMPLATE        PCF_BIT(6)
#define PCF_IN_TYPEDEF         PCF_BIT(7)
#define PCF_IN_CONST_ARGS      PCF_BIT(8)
#define PCF_IN_ARRAY_ASSIGN    PCF_BIT(9)
#define PCF_IN_CLASS           PCF_BIT(10)
#define PCF_IN_CLASS_BASE      PCF_BIT(11)
#define PCF_IN_NAMESPACE       PCF_BIT(12)
#define PCF_IN_FOR             PCF_BIT(13)
#define PCF_IN_OC_MSG          PCF_BIT(14)

/* Non-Copy flags are in the upper 48 bits */
#define PCF_FORCE_SPACE        PCF_BIT(16)  /* must have a space after this token */
#define PCF_STMT_START         PCF_BIT(17)  /* marks the start of a statement */
#define PCF_EXPR_START         PCF_BIT(18)
#define PCF_DONT_INDENT        PCF_BIT(19)  /* already aligned! */
#define PCF_ALIGN_START        PCF_BIT(20)
#define PCF_WAS_ALIGNED        PCF_BIT(21)
#define PCF_VAR_TYPE           PCF_BIT(22)  /* part of a variable def type */
#define PCF_VAR_DEF            PCF_BIT(23)  /* variable name in a variable def */
#define PCF_VAR_DECL           PCF_BIT(24)  /* variable name in a variable declaration */
#define PCF_VAR_INLINE         PCF_BIT(25)  /* type was an inline struct/enum/union */
#define PCF_RIGHT_COMMENT      PCF_BIT(26)
#define PCF_OLD_FCN_PARAMS     PCF_BIT(27)
#define PCF_LVALUE             PCF_BIT(28) /* left of assignment */
#define PCF_ONE_LINER          PCF_BIT(29)
#define PCF_ONE_CLASS          (PCF_ONE_LINER | PCF_IN_CLASS)
#define PCF_EMPTY_BODY         PCF_BIT(30)
#define PCF_ANCHOR             PCF_BIT(31)  /* aligning anchor */
#define PCF_PUNCTUATOR         PCF_BIT(32)
#define PCF_KEYWORD            PCF_BIT(33)  /* is a recognized keyword */
#define PCF_LONG_BLOCK         PCF_BIT(34)  /* the block is 'long' by some measure */
#define PCF_OC_BOXED           PCF_BIT(35)  /* inside OC boxed expression */
#define PCF_STATIC             PCF_BIT(36)  /* static storage class */
#define PCF_OC_RTYPE           PCF_BIT(37)  /* inside OC return type */
#define PCF_OC_ATYPE           PCF_BIT(38)  /* inside OC arg type */
#define PCF_DEF                PCF_BIT(39)  /* struct/union/enum/class/namespace definition */
#define PCF_PROTO              PCF_BIT(40)  /* struct/union/enum/class/namespace prototype */
#define PCF_REF                PCF_BIT(41)  /* struct/union/enum/class/namespace reference */
#define PCF_TYPEDEF_STRUCT     PCF_BIT(42)  /* typedef of a struct */
#define PCF_TYPEDEF_UNION      PCF_BIT(43)  /* typedef of a union */
#define PCF_TYPEDEF_ENUM       PCF_BIT(44)  /* typedef of an enum */


#ifdef DEFINE_PCF_NAMES
static const char *pcf_names[] =
{
   "IN_PREPROC",        // 0
   "IN_STRUCT",         // 1
   "IN_ENUM",           // 2
   "IN_FCN_DEF",        // 3
   "IN_FCN_CALL",       // 4
   "IN_SPAREN",         // 5
   "IN_TEMPLATE",       // 6
   "IN_TYPEDEF",        // 7
   "IN_CONST_ARGS",     // 8
   "IN_ARRAY_ASSIGN",   // 9
   "IN_CLASS",          // 10
   "IN_CLASS_BASE",     // 11
   "IN_NAMESPACE",      // 12
   "IN_FOR",            // 13
   "IN_OC_MSG",         // 14
   "#15",               // 15
   "FORCE_SPACE",       // 16
   "STMT_START",        // 17
   "EXPR_START",        // 18
   "DONT_INDENT",       // 19
   "ALIGN_START",       // 20
   "WAS_ALIGNED",       // 21
   "VAR_TYPE",          // 22
   "VAR_DEF",           // 23
   "VAR_DECL",          // 24
   "VAR_INLINE",        // 25
   "RIGHT_COMMENT",     // 26
   "OLD_FCN_PARAMS",    // 27
   "LVALUE",            // 28
   "ONE_LINER",         // 29
   "EMPTY_BODY",        // 30
   "ANCHOR",            // 31
   "PUNCTUATOR",        // 32
   "KEYWORD",           // 33
   "LONG_BLOCK",        // 34
   "OC_BOXED",          // 35
   "STATIC",            // 36
   "OC_RTYPE",          // 37
   "OC_ATYPE",          // 38
   "DEF",               // 39
   "PROTO",             // 40
   "REF",               // 41
   "TYPEDEF_STRUCT",    // 42
   "TYPEDEF_UNION",     // 43
   "TYPEDEF_ENUM",      // 44
};
#endif


/** This is the main type of this program */
struct chunk_t
{
   chunk_t()
   {
      reset();
   }
   void reset()
   {
      next = 0;
      prev = 0;
      type = CT_NONE;
      parent_type = CT_NONE;
      orig_line = 0;
      orig_col = 0;
      orig_col_end = 0;
      flags = 0;
      level = 0;
      brace_level = 0;
      pp_level = 0;
      str.clear();
      scope.clear();
   }
   int len()
   {
      return str.size();
   }
   const char *text()
   {
      return str.c_str();
   }
   const char *scope_text()
   {
      return scope.c_str();
   }

   chunk_t      *next;
   chunk_t      *prev;
   c_token_t    type;
   c_token_t    parent_type;      /* usually CT_NONE */
   UINT32       orig_line;
   UINT32       orig_col;
   UINT32       orig_col_end;
   UINT64       flags;            /* see PCF_xxx */
   int          level;            /* nest level in {, (, or [ */
   int          brace_level;      /* nest level in braces only */
   int          pp_level;         /* nest level in #if stuff */
   string       str;              /* the token text */
   string       scope;            /* the scope of the token */
};

enum
{
   LANG_NONE = 0x0000,
   LANG_C    = 0x0001,
   LANG_CPP  = 0x0002,
   LANG_D    = 0x0004,
   LANG_CS   = 0x0008,     /*<< C# or C-sharp */
   LANG_JAVA = 0x0010,
   LANG_OC   = 0x0020,     /*<< Objective C */
   LANG_VALA = 0x0040,     /*<< Like C# */
   LANG_PAWN = 0x0080,
   LANG_ECMA = 0x0100,

   LANG_ALLC = 0x017f,
   LANG_ALL  = 0x0fff,

   FLAG_PP   = 0x8000,     /*<< only appears in a preprocessor */
};

/**
 * Pattern classes for special keywords
 */
enum pattern_class
{
   PATCLS_NONE,
   PATCLS_BRACED,   // keyword + braced stmt:          do, try
   PATCLS_PBRACED,  // keyword + parens + braced stmt: switch, if, for, while
   PATCLS_OPBRACED, // keyword + optional parens + braced stmt: catch, version
   PATCLS_VBRACED,  // keyword + value + braced stmt: namespace
   PATCLS_PAREN,    // keyword + parens: while-of-do
   PATCLS_OPPAREN,  // keyword + optional parens: invariant (D lang)
   PATCLS_ELSE,     // Special case of PATCLS_BRACED for handling CT_IF
};

struct chunk_tag_t
{
   const char *tag;
   c_token_t  type;
   int        lang_flags;
};

struct lookup_entry_t
{
   char              ch;
   char              left_in_group;
   UINT16            next_idx;
   const chunk_tag_t *tag;
};

struct fp_data
{
   const char         *filename;
   vector<UINT8>      data;
   char               digest[33];

   struct parse_frame frames[16];
   int                frame_count;
   int                frame_pp_level;

   int                lang_flags; // LANG_xxx

   ListManager<chunk_t> chunk_list;
};

struct cp_data
{
   int                forced_lang_flags; // LANG_xxx
   sqlite3            *index;

   sqlite3_stmt       *stmt_insert_reference;
   sqlite3_stmt       *stmt_insert_definition;
   sqlite3_stmt       *stmt_insert_declaration;

   sqlite3_stmt       *stmt_begin;
   sqlite3_stmt       *stmt_commit;
   sqlite3_stmt       *stmt_insert_file;
   sqlite3_stmt       *stmt_remove_file;
   sqlite3_stmt       *stmt_prune_refs;
   sqlite3_stmt       *stmt_prune_defs;
   sqlite3_stmt       *stmt_prune_decls;
   sqlite3_stmt       *stmt_change_digest;
   sqlite3_stmt       *stmt_lookup_file;
};

extern struct cp_data cpd;

typedef enum
{
   IT_IDENTIFIER,        // Unspecified identifier
   IT_MACRO,             // preprocessor macro
   IT_MACRO_FUNCTION,    // function like preprocessor macro
   IT_FUNCTION,          // functions
   IT_STRUCT,            // struct <tag>
   IT_UNION,             // union <tag>
   IT_ENUM,              // enum <tag>
   IT_ENUM_VAL,          // values of an enum
   IT_CLASS,             // class
   IT_STRUCT_TYPE,       // typedef alias of a struct
   IT_UNION_TYPE,        // typedef alias of a union
   IT_ENUM_TYPE,         // typedef alias of an enum
   IT_FUNCTION_TYPE,     // typedef of a function or function ptr
   IT_TYPE,              // a type
   IT_VAR,               // a variable
   IT_NAMESPACE,         // a namespace
} id_type;

typedef enum
{
   IST_REFERENCE,
   IST_DEFINITION,
   IST_DECLARATION,
} id_sub_type;

#endif   /* TOKS_TYPES_H_INCLUDED */
