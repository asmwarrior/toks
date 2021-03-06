/**
 * @file chunk_list.c
 * Manages and navigates the list of chunks.
 *
 * @author  Ben Gardner
 * @license GPL v2+
 */
#ifndef CHUNK_LIST_H_INCLUDED
#define CHUNK_LIST_H_INCLUDED

#include "toks_types.h"
#include "char_table.h"


/**
 * Specifies how to handle proprocessors.
 * CNAV_ALL (default)
 *  - return the true next/prev
 *
 * CNAV_PREPROC
 *  - If not in a preprocessor, skip over any encountered preprocessor stuff
 *  - If in a preprocessor, fail to leave (return NULL)
 */
enum chunk_nav_t
{
   CNAV_ALL,
   CNAV_PREPROC,
};


chunk_t *chunk_dup(const chunk_t *pc_in);

chunk_t *chunk_add(fp_data& fpd, const chunk_t *pc_in);
chunk_t *chunk_add_after(fp_data& fpd, const chunk_t *pc_in, chunk_t *ref);
chunk_t *chunk_add_before(fp_data& fpd, const chunk_t *pc_in, chunk_t *ref);

void chunk_del(fp_data& fpd, chunk_t *pc);

chunk_t *chunk_get_head(fp_data& fpd);
chunk_t *chunk_get_tail(fp_data& fpd);
chunk_t *chunk_get_next(chunk_t *cur, chunk_nav_t nav = CNAV_ALL);
chunk_t *chunk_get_prev(chunk_t *cur, chunk_nav_t nav = CNAV_ALL);

chunk_t *chunk_first_on_line(chunk_t *pc);

chunk_t *chunk_get_next_nl(chunk_t *cur, chunk_nav_t nav = CNAV_ALL);
chunk_t *chunk_get_next_nnl(chunk_t *cur, chunk_nav_t nav = CNAV_ALL);
chunk_t *chunk_get_next_nnlnp(chunk_t *cur, chunk_nav_t nav = CNAV_ALL);

chunk_t *chunk_get_prev_nl(chunk_t *cur, chunk_nav_t nav = CNAV_ALL);
chunk_t *chunk_get_prev_nnl(chunk_t *cur, chunk_nav_t nav = CNAV_ALL);
chunk_t *chunk_get_prev_nnlnp(chunk_t *cur, chunk_nav_t nav = CNAV_ALL);

chunk_t *chunk_get_next_type(chunk_t *cur, c_token_t type, int level, chunk_nav_t nav = CNAV_ALL);
chunk_t *chunk_get_prev_type(chunk_t *cur, c_token_t type, int level, chunk_nav_t nav = CNAV_ALL);

chunk_t *chunk_get_next_str(chunk_t *cur, const char *str, int len, int level, chunk_nav_t nav = CNAV_ALL);
chunk_t *chunk_get_prev_str(chunk_t *cur, const char *str, int len, int level, chunk_nav_t nav = CNAV_ALL);


/**
 * Skips to the closing match for the current paren/brace/square.
 *
 * @param cur  The opening or closing paren/brace/square
 * @return     NULL or the matching paren/brace/square
 */
static_inline
chunk_t *chunk_skip_to_match(chunk_t *cur, chunk_nav_t nav = CNAV_ALL)
{
   if (cur &&
       ((cur->type == CT_PAREN_OPEN) ||
        (cur->type == CT_SPAREN_OPEN) ||
        (cur->type == CT_FPAREN_OPEN) ||
        (cur->type == CT_TPAREN_OPEN) ||
        (cur->type == CT_BRACE_OPEN) ||
        (cur->type == CT_VBRACE_OPEN) ||
        (cur->type == CT_ANGLE_OPEN) ||
        (cur->type == CT_SQUARE_OPEN)))
   {
      return chunk_get_next_type(cur, (c_token_t)(cur->type + 1), cur->level, nav);
   }
   return cur;
}


static_inline
chunk_t *chunk_skip_to_match_rev(chunk_t *cur, chunk_nav_t nav = CNAV_ALL)
{
   if (cur &&
       ((cur->type == CT_PAREN_CLOSE) ||
        (cur->type == CT_SPAREN_CLOSE) ||
        (cur->type == CT_FPAREN_CLOSE) ||
        (cur->type == CT_TPAREN_CLOSE) ||
        (cur->type == CT_BRACE_CLOSE) ||
        (cur->type == CT_VBRACE_CLOSE) ||
        (cur->type == CT_ANGLE_CLOSE) ||
        (cur->type == CT_SQUARE_CLOSE)))
   {
      return chunk_get_prev_type(cur, (c_token_t)(cur->type - 1), cur->level, nav);
   }
   return cur;
}


static_inline
bool chunk_is_newline(chunk_t *pc)
{
   return((pc != NULL) && ((pc->type == CT_NEWLINE) ||
                           (pc->type == CT_NL_CONT)));
}


static_inline
bool chunk_is_semicolon(chunk_t *pc)
{
   return((pc != NULL) && ((pc->type == CT_SEMICOLON) ||
                           (pc->type == CT_VSEMICOLON)));
}


static_inline
bool chunk_is_blank(chunk_t *pc)
{
   return((pc != NULL) && (pc->len() == 0));
}


static_inline
bool chunk_is_preproc(chunk_t *pc)
{
   return((pc != NULL) && ((pc->flags & PCF_IN_PREPROC) != 0));
}


static_inline
bool chunk_is_type(chunk_t *pc)
{
   return((pc != NULL) && ((pc->type == CT_TYPE) ||
                           (pc->type == CT_PTR_TYPE) ||
                           (pc->type == CT_BYREF) ||
                           (pc->type == CT_DC_MEMBER) ||
                           (pc->type == CT_QUALIFIER) ||
                           (pc->type == CT_STRUCT) ||
                           (pc->type == CT_ENUM) ||
                           (pc->type == CT_UNION)));
}


static_inline
bool chunk_is_token(chunk_t *pc, c_token_t c_token)
{
   return((pc != NULL) && (pc->type == c_token));
}


static_inline
bool chunk_is_str(chunk_t *pc, const char *str, int len)
{
   return((pc != NULL) && (pc->len() == len) && (memcmp(pc->text(), str, len) == 0));
}


static_inline
bool chunk_is_str_case(chunk_t *pc, const char *str, int len)
{
   return((pc != NULL) && (pc->len() == len) && (strncasecmp(pc->text(), str, len) == 0));
}


static_inline
bool chunk_is_word(chunk_t *pc)
{
   return((pc != NULL) && (pc->len() >= 1) && CharTable::IsKw1(pc->str[0]));
}


static_inline
bool chunk_is_star(chunk_t *pc)
{
   return((pc != NULL) && (pc->len() == 1) && (pc->str[0] == '*') && (pc->type != CT_OPERATOR_VAL));
}


static_inline
bool chunk_is_addr(chunk_t *pc)
{
   return((pc != NULL) &&
          ((pc->type == CT_BYREF) ||
           ((pc->len() == 1) && (pc->str[0] == '&') && (pc->type != CT_OPERATOR_VAL))));
}


bool chunk_is_newline_between(chunk_t *start, chunk_t *end);

static_inline
bool chunk_is_closing_brace(chunk_t *pc)
{
   return((pc != NULL) && ((pc->type == CT_BRACE_CLOSE) ||
                           (pc->type == CT_VBRACE_CLOSE)));
}


static_inline
bool chunk_is_opening_brace(chunk_t *pc)
{
   return((pc != NULL) && ((pc->type == CT_BRACE_OPEN) ||
                           (pc->type == CT_VBRACE_OPEN)));
}


static_inline
bool chunk_is_vbrace(chunk_t *pc)
{
   return((pc != NULL) && ((pc->type == CT_VBRACE_CLOSE) ||
                           (pc->type == CT_VBRACE_OPEN)));
}


static_inline
bool chunk_is_paren_open(chunk_t *pc)
{
   return((pc != NULL) &&
          ((pc->type == CT_PAREN_OPEN) ||
           (pc->type == CT_SPAREN_OPEN) ||
           (pc->type == CT_TPAREN_OPEN) ||
           (pc->type == CT_FPAREN_OPEN)));
}


static_inline
bool chunk_is_paren_close(chunk_t *pc)
{
   return((pc != NULL) &&
          ((pc->type == CT_PAREN_CLOSE) ||
           (pc->type == CT_SPAREN_CLOSE) ||
           (pc->type == CT_TPAREN_CLOSE) ||
           (pc->type == CT_FPAREN_CLOSE)));
}


/**
 * Returns true if either chunk is null or both have the same preproc flags.
 * If this is true, you can remove a newline/nl_cont between the two.
 */
static_inline
bool chunk_same_preproc(chunk_t *pc1, chunk_t *pc2)
{
   return((pc1 == NULL) || (pc2 == NULL) ||
          ((pc1->flags & PCF_IN_PREPROC) == (pc2->flags & PCF_IN_PREPROC)));
}

#endif   /* CHUNK_LIST_H_INCLUDED */
