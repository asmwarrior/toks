/**
 * @file combine.cpp
 * Labels the chunks as needed.
 *
 * @author  Ben Gardner
 * @license GPL v2+
 */
#include "toks_types.h"
#include "chunk_list.h"
#include "ChunkStack.h"
#include "prototypes.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <cassert>

static void fix_fcn_def_params(fp_data& fpd, chunk_t *pc);
static void fix_typedef(fp_data& fpd, chunk_t *pc);
static void fix_enum_struct_union(fp_data& fpd, chunk_t *pc);
static void fix_casts(chunk_t *pc);
static void fix_type_cast(chunk_t *pc);
static chunk_t *fix_var_def(chunk_t *pc);
static void mark_function(fp_data& fpd, chunk_t *pc);
static void mark_function_return_type(chunk_t *fname, chunk_t *pc, c_token_t parent_type);
static bool mark_function_type(fp_data& fpd, chunk_t *pc);
static void mark_struct_union_body(chunk_t *start);
static chunk_t *mark_variable_definition(chunk_t *start, UINT64 flags);

static void mark_define_expressions(fp_data& fpd);
static void mark_class_ctor(fp_data& fpd, chunk_t *pclass);
static void mark_namespace(chunk_t *pns);
static void mark_cpp_constructor(fp_data& fpd, chunk_t *pc);
static void mark_lvalue(chunk_t *pc);
static void mark_template_func(fp_data& fpd, chunk_t *pc, chunk_t *pc_next);
static void mark_exec_sql(chunk_t *pc);
static void handle_oc_class(chunk_t *pc);
static void handle_oc_block_literal(fp_data& fpd, chunk_t *pc);
static void handle_oc_block_type(fp_data& fpd, chunk_t *pc);
static void handle_oc_message_decl(chunk_t *pc);
static void handle_oc_message_send(chunk_t *pc);
static void handle_cs_square_stmt(chunk_t *pc);
static void handle_cs_property(chunk_t *pc);
static void handle_cpp_template(chunk_t *pc);
static void handle_cpp_lambda(fp_data& fpd, chunk_t *pc);
static void handle_d_template(chunk_t *pc);
static void handle_wrap(fp_data& fpd, chunk_t *pc);
static void handle_proto_wrap(fp_data& fpd, chunk_t *pc);
static bool is_oc_block(chunk_t *pc);
static void handle_java_assert(chunk_t *pc);
static chunk_t *get_d_template_types(ChunkStack& cs, chunk_t *open_paren);
static bool chunkstack_match(ChunkStack& cs, chunk_t *pc);

void make_type(chunk_t *pc)
{
   if (pc != NULL)
   {
      if (pc->type == CT_WORD)
      {
         pc->type = CT_TYPE;
      }
      else if (chunk_is_star(pc))
      {
         pc->type = CT_PTR_TYPE;
      }
      else if (chunk_is_addr(pc))
      {
         pc->type = CT_BYREF;
      }
   }
}


void flag_series(chunk_t *start, chunk_t *end, UINT64 set_flags, UINT64 clr_flags, chunk_nav_t nav)
{
   while (start && (start != end))
   {
      start->flags = (start->flags & ~clr_flags) | set_flags;

      start = chunk_get_next(start, nav);
   }
   if (end)
   {
      end->flags = (end->flags & ~clr_flags) | set_flags;
   }
}


/**
 * Flags everything from the open paren to the close paren.
 *
 * @param po   Pointer to the open parenthesis
 * @return     The token after the close paren
 */
#define flag_parens(_po, _flg, _ot, _pt, _pa) \
   flag_parens2(__func__, __LINE__, _po, _flg, _ot, _pt, _pa)

static chunk_t *flag_parens2(const char *func, int line,
                             chunk_t *po, UINT64 flags,
                             c_token_t opentype, c_token_t parenttype,
                             bool parent_all)
{
   chunk_t *paren_close;
   chunk_t *pc;

   paren_close = chunk_skip_to_match(po, CNAV_PREPROC);
   if (paren_close == NULL)
   {
      LOG_FMT(LWARN, "flag_parens[%s:%d]: no match for [%s] at  [%d:%d]\n",
              func, line, po->str.c_str(), po->orig_line, po->orig_col);
      return(NULL);
   }

   LOG_FMT(LFLPAREN, "flag_parens[%s:%d] @ %d:%d [%s] and %d:%d [%s] type=%s ptype=%s\n",
           func, line, po->orig_line, po->orig_col, po->text(),
           paren_close->orig_line, paren_close->orig_col, paren_close->text(),
           get_token_name(opentype), get_token_name(parenttype));

   if (po != paren_close)
   {
      if ((flags != 0) ||
          (parent_all && (parenttype != CT_NONE)))
      {
         for (pc = chunk_get_next(po, CNAV_PREPROC);
              pc != paren_close;
              pc = chunk_get_next(pc, CNAV_PREPROC))
         {
            pc->flags |= flags;
            if (parent_all)
            {
               pc->parent_type = parenttype;
            }
         }
      }

      if (opentype != CT_NONE)
      {
         po->type          = opentype;
         paren_close->type = (c_token_t)(opentype + 1);
      }

      if (parenttype != CT_NONE)
      {
         po->parent_type          = parenttype;
         paren_close->parent_type = parenttype;
      }
   }
   return(chunk_get_next_nnl(paren_close, CNAV_PREPROC));
}


/**
 * Sets the parent of the open paren/brace/square/angle and the closing.
 * Note - it is assumed that pc really does point to an open item and the
 * close must be open + 1.
 *
 * @param start   The open paren
 * @param parent  The type to assign as the parent
 * @reutrn        The chunk after the close paren
 */
chunk_t *set_paren_parent(chunk_t *start, c_token_t parent)
{
   chunk_t *end;

   end = chunk_skip_to_match(start, CNAV_PREPROC);
   if (end != NULL)
   {
      start->parent_type = parent;
      end->parent_type   = parent;
   }
   return(chunk_get_next_nnl(end, CNAV_PREPROC));
}


/* Scan backwards to see if we might be on a type declaration */
static bool chunk_ends_type(chunk_t *pc)
{
   bool ret = false;
   int  cnt = 0;
   bool last_lval = false;

   for (/* nada */; pc != NULL; pc = chunk_get_prev_nnl(pc))
   {
      LOG_FMT(LFTYPE, "%s: [%s] %s flags %" PRIx64 " on line %d, col %d\n",
              __func__, get_token_name(pc->type), pc->str.c_str(),
              pc->flags, pc->orig_line, pc->orig_col);

      if ((pc->type == CT_WORD) ||
          (pc->type == CT_TYPE) ||
          (pc->type == CT_PTR_TYPE) ||
          (pc->type == CT_STRUCT) ||
          (pc->type == CT_DC_MEMBER) ||
          (pc->type == CT_QUALIFIER))
      {
         cnt++;
         last_lval = (pc->flags & PCF_LVALUE) != 0;
         continue;
      }

      if (chunk_is_semicolon(pc) ||
          (pc->type == CT_TYPEDEF) ||
          (pc->type == CT_BRACE_OPEN) ||
          (pc->type == CT_BRACE_CLOSE) ||
          ((pc->type == CT_SPAREN_OPEN) && last_lval))
      {
         ret = cnt > 0;
      }
      break;
   }

   if (pc == NULL)
   {
      /* first token */
      ret = true;
   }

   LOG_FMT(LFTYPE, "%s verdict: %s\n", __func__, ret ? "yes" : "no");

   return(ret);
}


/* skip to the final word/type in a :: chain
 * pc is either a word or a ::
 */
static chunk_t *skip_dc_member(chunk_t *start)
{
   if (!start)
   {
      return NULL;
   }

   chunk_t *pc   = start;
   chunk_t *next = (pc->type == CT_DC_MEMBER) ? pc : chunk_get_next_nnl(pc);
   while (next && (next->type == CT_DC_MEMBER))
   {
      pc   = chunk_get_next_nnl(next);
      next = chunk_get_next_nnl(pc);
   }
   return pc;
}


/**
 * This is called on every chunk.
 * First on all non-preprocessor chunks and then on each preprocessor chunk.
 * It does all the detection and classifying.
 */
static void do_symbol_check(fp_data& fpd, chunk_t *prev, chunk_t *pc, chunk_t *next)
{
   chunk_t *tmp;

   // LOG_FMT(LNOTE, " %3d > ['%s' %s] ['%s' %s] ['%s' %s]\n",
   //         pc->orig_line,
   //         prev->str.c_str(), get_token_name(prev->type),
   //         pc->str.c_str(), get_token_name(pc->type),
   //         next->str.c_str(), get_token_name(next->type));

   if ((pc->type == CT_OC_AT) && next)
   {
      if ((next->type == CT_PAREN_OPEN) ||
          (next->type == CT_BRACE_OPEN) ||
          (next->type == CT_SQUARE_OPEN))
      {
         flag_parens(next, PCF_OC_BOXED, next->type, CT_OC_AT, false);
      }
      else
      {
         next->parent_type = CT_OC_AT;
      }
   }

   /* D stuff */
   if ((fpd.lang_flags & LANG_D) &&
       (pc->type == CT_QUALIFIER) &&
       chunk_is_str(pc, "const", 5) &&
       (next->type == CT_PAREN_OPEN))
   {
      pc->type = CT_D_CAST;
      set_paren_parent(next, pc->type);
   }

   if ((next->type == CT_PAREN_OPEN) &&
       ((pc->type == CT_D_CAST) ||
        (pc->type == CT_DELEGATE) ||
        (pc->type == CT_ALIGN)))
   {
      /* mark the parenthesis parent */
      tmp = set_paren_parent(next, pc->type);

      /* For a D cast - convert the next item */
      if ((pc->type == CT_D_CAST) && (tmp != NULL))
      {
         if (tmp->type == CT_STAR)
         {
            tmp->type = CT_DEREF;
         }
         else if (tmp->type == CT_AMP)
         {
            tmp->type = CT_ADDR;
         }
         else if (tmp->type == CT_MINUS)
         {
            tmp->type = CT_NEG;
         }
         else if (tmp->type == CT_PLUS)
         {
            tmp->type = CT_POS;
         }
      }

      /* For a delegate, mark previous words as types and the item after the
       * close paren as a variable def
       */
      if (pc->type == CT_DELEGATE)
      {
         if (tmp != NULL)
         {
            tmp->parent_type = CT_DELEGATE;
            if (tmp->level == tmp->brace_level)
            {
               tmp->flags |= PCF_VAR_DEF;
            }
         }

         for (tmp = chunk_get_prev_nnl(pc); tmp != NULL; tmp = chunk_get_prev_nnl(tmp))
         {
            if (chunk_is_semicolon(tmp) ||
                (tmp->type == CT_BRACE_OPEN) ||
                (tmp->type == CT_VBRACE_OPEN))
            {
               break;
            }
            make_type(tmp);
         }
      }

      if ((pc->type == CT_ALIGN) && (tmp != NULL))
      {
         if (tmp->type == CT_BRACE_OPEN)
         {
            set_paren_parent(tmp, pc->type);
         }
         else if (tmp->type == CT_COLON)
         {
            tmp->parent_type = pc->type;
         }
      }
   } /* paren open + cast/align/delegate */

   if (pc->type == CT_INVARIANT)
   {
      if (next->type == CT_PAREN_OPEN)
      {
         next->parent_type = pc->type;
         tmp = chunk_get_next(next);
         while (tmp != NULL)
         {
            if (tmp->type == CT_PAREN_CLOSE)
            {
               tmp->parent_type = pc->type;
               break;
            }
            make_type(tmp);
            tmp = chunk_get_next(tmp);
         }
      }
      else
      {
         pc->type = CT_QUALIFIER;
      }
   }

   if ((prev->type == CT_BRACE_OPEN) &&
       ((pc->type == CT_GETSET) || (pc->type == CT_GETSET_EMPTY)))
   {
      flag_parens(prev, 0, CT_NONE, CT_GETSET, false);
   }

   /* Objective C stuff */
   if (fpd.lang_flags & LANG_OC)
   {
      /* Check for message declarations */
      if (pc->flags & PCF_STMT_START)
      {
         if ((chunk_is_str(pc, "-", 1) || chunk_is_str(pc, "+", 1)) &&
             chunk_is_str(next, "(", 1))
         {
            handle_oc_message_decl(pc);
         }
      }
      if (pc->flags & PCF_EXPR_START)
      {
         if (pc->type == CT_SQUARE_OPEN)
         {
            handle_oc_message_send(pc);
         }
         if (pc->type == CT_CARET)
         {
            handle_oc_block_literal(fpd, pc);
         }
      }
   }

   /* C# stuff */
   if (fpd.lang_flags & LANG_CS)
   {
      /* '[assembly: xxx]' stuff */
      if ((pc->flags & PCF_EXPR_START) &&
          (pc->type == CT_SQUARE_OPEN))
      {
         handle_cs_square_stmt(pc);
      }

      if ((next != NULL) && (next->type == CT_BRACE_OPEN) &&
          (next->parent_type == CT_NONE) &&
          ((pc->type == CT_SQUARE_CLOSE) ||
           (pc->type == CT_WORD)))
      {
         handle_cs_property(next);
      }
   }

   /* C++11 Lambda stuff */
   if (prev && (fpd.lang_flags & LANG_CPP) &&
       ((pc->type == CT_SQUARE_OPEN) || (pc->type == CT_TSQUARE)) &&
       !CharTable::IsKw1(prev->str[0]))
   {
      handle_cpp_lambda(fpd, pc);
   }

   /* FIXME: which language does this apply to? */
   if ((pc->type == CT_ASSIGN) && (next->type == CT_SQUARE_OPEN))
   {
      set_paren_parent(next, CT_ASSIGN);

      /* Mark one-liner assignment */
      tmp = next;
      while ((tmp = chunk_get_next(tmp)) != NULL)
      {
         if (chunk_is_newline(tmp))
         {
            break;
         }
         if ((tmp->type == CT_SQUARE_CLOSE) && (next->level == tmp->level))
         {
            tmp->flags  |= PCF_ONE_LINER;
            next->flags |= PCF_ONE_LINER;
            break;
         }
      }
   }

   if (pc->type == CT_ASSERT)
   {
      handle_java_assert(pc);
   }
   if (pc->type == CT_ANNOTATION)
   {
       tmp = chunk_get_next_nnl(pc);
       if (chunk_is_paren_open(tmp))
       {
          set_paren_parent(tmp, CT_ANNOTATION);
       }
   }

   /* A [] in C# and D only follows a type */
   if ((pc->type == CT_TSQUARE) &&
       ((fpd.lang_flags & (LANG_D | LANG_CS | LANG_VALA)) != 0))
   {
      if ((prev != NULL) && (prev->type == CT_WORD))
      {
         prev->type = CT_TYPE;
      }
      if ((next != NULL) && (next->type == CT_WORD))
      {
         next->flags |= PCF_VAR_DEF;
      }
   }

   if ((pc->type == CT_SQL_EXEC) ||
       (pc->type == CT_SQL_BEGIN) ||
       (pc->type == CT_SQL_END))
   {
      mark_exec_sql(pc);
   }

   if (pc->type == CT_PROTO_WRAP)
   {
      handle_proto_wrap(fpd, pc);
   }

   /* Handle the typedef */
   if (pc->type == CT_TYPEDEF)
   {
      fix_typedef(fpd, pc);
   }
   if ((pc->type == CT_ENUM) ||
       (pc->type == CT_STRUCT) ||
       (pc->type == CT_UNION))
   {
      fix_enum_struct_union(fpd, pc);
   }

   if (pc->type == CT_EXTERN)
   {
      if (chunk_is_paren_open(next))
      {
         tmp = flag_parens(next, 0, CT_NONE, CT_EXTERN, true);
         if (tmp && (tmp->type == CT_BRACE_OPEN))
         {
            set_paren_parent(tmp, CT_EXTERN);
         }
      }
      else
      {
         /* next likely is a string (see tokenize_cleanup.cpp) */
         next->parent_type = CT_EXTERN;
         tmp = chunk_get_next_nnl(next);
         if (tmp && (tmp->type == CT_BRACE_OPEN))
         {
            set_paren_parent(tmp, CT_EXTERN);
         }
      }
   }

   if (pc->type == CT_TEMPLATE)
   {
      if (fpd.lang_flags & LANG_D)
      {
         handle_d_template(pc);
      }
      else
      {
         handle_cpp_template(pc);
      }
   }

   if ((pc->type == CT_WORD) &&
       (next->type == CT_ANGLE_OPEN) &&
       (next->parent_type == CT_TEMPLATE))
   {
      mark_template_func(fpd, pc, next);
   }

   if ((pc->type == CT_SQUARE_CLOSE) &&
       (next->type == CT_PAREN_OPEN))
   {
      flag_parens(next, 0, CT_FPAREN_OPEN, CT_NONE, false);
   }

   if (pc->type == CT_TYPE_CAST)
   {
      fix_type_cast(pc);
   }

   if ((pc->parent_type == CT_ASSIGN) &&
       ((pc->type == CT_BRACE_OPEN) ||
        (pc->type == CT_SQUARE_OPEN)))
   {
      /* Mark everything in here as in assign */
      flag_parens(pc, PCF_IN_ARRAY_ASSIGN, pc->type, CT_NONE, false);
   }

   if (pc->type == CT_D_TEMPLATE)
   {
      set_paren_parent(next, pc->type);
   }

   /**
    * A word before an open paren is a function call or definition.
    * CT_WORD => CT_FUNC_CALL or CT_FUNC_DEF
    */
   if (next->type == CT_PAREN_OPEN)
   {
      tmp = chunk_get_next_nnl(next);
      if ((fpd.lang_flags & LANG_OC) && chunk_is_token(tmp, CT_CARET))
      {
         handle_oc_block_type(fpd, tmp);
      }
      else if ((pc->type == CT_WORD) || (pc->type == CT_OPERATOR_VAL))
      {
         pc->type = CT_FUNCTION;
      }
      else if (pc->type == CT_TYPE)
      {
         /**
          * If we are on a type, then we are either on a C++ style cast, a
          * function or we are on a function type.
          * The only way to tell for sure is to find the close paren and see
          * if it is followed by an open paren.
          * "int(5.6)"
          * "int()"
          * "int(foo)(void)"
          *
          * FIXME: this check can be done better...
          */
         tmp = chunk_get_next_type(next, CT_PAREN_CLOSE, next->level);
         tmp = chunk_get_next(tmp);
         if ((tmp != NULL) && (tmp->type == CT_PAREN_OPEN))
         {
            /* we have "TYPE(...)(" */
            pc->type = CT_FUNCTION;
         }
         else
         {
            if ((pc->parent_type == CT_NONE) &&
                ((pc->flags & PCF_IN_TYPEDEF) == 0))
            {
               tmp = chunk_get_next_nnl(next);
               if ((tmp != NULL) && (tmp->type == CT_PAREN_CLOSE))
               {
                  /* we have TYPE() */
                  pc->type = CT_FUNCTION;
               }
               else
               {
                  /* we have TYPE(...) */
                  pc->type = CT_CPP_CAST;
                  set_paren_parent(next, CT_CPP_CAST);
               }
            }
         }
      }
      else if (pc->type == CT_ATTRIBUTE)
      {
         flag_parens(next, 0, CT_FPAREN_OPEN, CT_ATTRIBUTE, false);
      }
   }
   if ((fpd.lang_flags & LANG_PAWN) != 0)
   {
      if ((pc->type == CT_FUNCTION) && (pc->brace_level > 0))
      {
         pc->type = CT_FUNC_CALL;
      }
      if ((pc->type == CT_STATE) &&
          (next != NULL) &&
          (next->type == CT_PAREN_OPEN))
      {
         set_paren_parent(next, pc->type);
      }
   }
   else
   {
      if ((pc->type == CT_FUNCTION) &&
          ((pc->parent_type == CT_OC_BLOCK_EXPR) || !is_oc_block(pc)))
      {
         mark_function(fpd, pc);
      }
   }

   /* Detect C99 member stuff */
   if ((pc->type == CT_MEMBER) &&
       ((prev->type == CT_COMMA) ||
        (prev->type == CT_BRACE_OPEN)))
   {
      pc->type          = CT_C99_MEMBER;
      next->parent_type = CT_C99_MEMBER;
   }

   /* Mark function parens and braces */
   if ((pc->type == CT_FUNC_DEF) ||
       (pc->type == CT_FUNC_CALL) ||
       (pc->type == CT_FUNC_CALL_USER) ||
       (pc->type == CT_FUNC_PROTO))
   {
      tmp = next;
      if (tmp->type == CT_SQUARE_OPEN)
      {
         tmp = set_paren_parent(tmp, pc->type);
      }
      else if ((tmp->type == CT_TSQUARE) ||
               (tmp->parent_type == CT_OPERATOR))
      {
         tmp = chunk_get_next_nnl(tmp);
      }

      if (chunk_is_paren_open(tmp))
      {
         tmp = flag_parens(tmp, 0, CT_FPAREN_OPEN, pc->type, false);
         if (tmp != NULL)
         {
            if (tmp->type == CT_BRACE_OPEN)
            {
               if ((pc->flags & PCF_IN_CONST_ARGS) == 0)
               {
                  set_paren_parent(tmp, pc->type);
               }
            }
            else if (chunk_is_semicolon(tmp) && (pc->type == CT_FUNC_PROTO))
            {
               tmp->parent_type = pc->type;
            }
         }
      }
   }

   /* Mark the parameters in catch() */
   if ((pc->type == CT_CATCH) && (next->type == CT_SPAREN_OPEN))
   {
      fix_fcn_def_params(fpd, next);
   }

   if ((pc->type == CT_THROW) && (prev->type == CT_FPAREN_CLOSE))
   {
      pc->parent_type = prev->parent_type;
      if (next->type == CT_PAREN_OPEN)
      {
         set_paren_parent(next, CT_THROW);
      }
   }

   /* Mark the braces in: "for_each_entry(xxx) { }" */
   if ((pc->type == CT_BRACE_OPEN) &&
       (prev->type == CT_FPAREN_CLOSE) &&
       ((prev->parent_type == CT_FUNC_CALL) ||
        (prev->parent_type == CT_FUNC_CALL_USER)) &&
       ((pc->flags & PCF_IN_CONST_ARGS) == 0))
   {
      set_paren_parent(pc, CT_FUNC_CALL);
   }

   /* Check for a close paren followed by an open paren, which means that
    * we are on a function type declaration (C/C++ only?).
    * Note that typedefs are already taken care of.
    */
   if ((next != NULL) &&
       ((pc->flags & (PCF_IN_TYPEDEF | PCF_IN_TEMPLATE)) == 0) &&
       (pc->parent_type != CT_CPP_CAST) &&
       (pc->parent_type != CT_C_CAST) &&
       ((pc->flags & PCF_IN_PREPROC) == 0) &&
       (!is_oc_block(pc)) &&
       (pc->parent_type != CT_OC_MSG_DECL) &&
       (pc->parent_type != CT_OC_MSG_SPEC) &&
       chunk_is_str(pc, ")", 1) &&
       chunk_is_str(next, "(", 1))
   {
      if ((fpd.lang_flags & LANG_D) != 0)
      {
         flag_parens(next, 0, CT_FPAREN_OPEN, CT_FUNC_CALL, false);
      }
      else
      {
         mark_function_type(fpd, pc);
      }
   }

   if (((pc->type == CT_CLASS) ||
        (pc->type == CT_STRUCT)) &&
       (pc->level == pc->brace_level))
   {
      if ((pc->type != CT_STRUCT) || ((fpd.lang_flags & LANG_C) == 0))
      {
         mark_class_ctor(fpd, pc);
      }
   }

   if (pc->type == CT_OC_CLASS)
   {
      handle_oc_class(pc);
   }

   if (pc->type == CT_NAMESPACE)
   {
      mark_namespace(pc);
   }

   /*TODO: Check for stuff that can only occur at the start of an statement */

   if ((fpd.lang_flags & LANG_D) == 0)
   {
      /**
       * Check a paren pair to see if it is a cast.
       * Note that SPAREN and FPAREN have already been marked.
       */
      if ((pc->type == CT_PAREN_OPEN) &&
          ((pc->parent_type == CT_NONE) ||
           (pc->parent_type == CT_OC_MSG) ||
           (pc->parent_type == CT_OC_BLOCK_EXPR)) &&
          ((next->type == CT_WORD) ||
           (next->type == CT_TYPE) ||
           (next->type == CT_STRUCT) ||
           (next->type == CT_QUALIFIER) ||
           (next->type == CT_MEMBER) ||
           (next->type == CT_DC_MEMBER) ||
           (next->type == CT_ENUM) ||
           (next->type == CT_UNION)) &&
          (prev->type != CT_SIZEOF) &&
          (prev->parent_type != CT_OPERATOR))
      {
         fix_casts(pc);
      }
   }


   /* Check for stuff that can only occur at the start of an expression */
   if ((pc->flags & PCF_EXPR_START) != 0)
   {
      /* Change STAR, MINUS, and PLUS in the easy cases */
      if (pc->type == CT_STAR)
      {
         pc->type = (prev->type == CT_ANGLE_CLOSE) ? CT_PTR_TYPE : CT_DEREF;
      }
      if (pc->type == CT_MINUS)
      {
         pc->type = CT_NEG;
      }
      if (pc->type == CT_PLUS)
      {
         pc->type = CT_POS;
      }
      if (pc->type == CT_INCDEC_AFTER)
      {
         pc->type = CT_INCDEC_BEFORE;
         //fprintf(stderr, "%s: %d> changed INCDEC_AFTER to INCDEC_BEFORE\n", __func__, pc->orig_line);
      }
      if (pc->type == CT_AMP)
      {
         //fprintf(stderr, "Changed AMP to ADDR on line %d\n", pc->orig_line);
         pc->type = CT_ADDR;
      }
      if (pc->type == CT_CARET)
      {
         if (fpd.lang_flags & LANG_OC)
         {
            /* This is likely the start of a block literal */
            handle_oc_block_literal(fpd, pc);
         }
      }
   }

   /* Detect a variable definition that starts with struct/enum/union/class */
   if (((pc->flags & PCF_IN_TYPEDEF) == 0) &&
       (prev->parent_type != CT_CPP_CAST) &&
       ((prev->flags & PCF_IN_FCN_DEF) == 0) &&
       ((pc->type == CT_STRUCT) ||
        (pc->type == CT_UNION) ||
        (pc->type == CT_CLASS) ||
        (pc->type == CT_ENUM)))
   {
      tmp = skip_dc_member(next);
      if (tmp && ((tmp->type == CT_TYPE) || (tmp->type == CT_WORD)))
      {
         tmp->parent_type = pc->type;
         tmp->type        = CT_TYPE;

         tmp = chunk_get_next_nnl(tmp);
      }
      if ((tmp != NULL) && (tmp->type == CT_BRACE_OPEN))
      {
         tmp = chunk_skip_to_match(tmp);
         tmp = chunk_get_next_nnl(tmp);
      }
      if ((tmp != NULL) && (chunk_is_star(tmp) || chunk_is_addr(tmp) || (tmp->type == CT_WORD)))
      {
         mark_variable_definition(tmp, PCF_VAR_DEF);
      }
   }

   if (pc->type == CT_OC_PROPERTY)
   {
      tmp = chunk_get_next_nnl(pc);
      if (chunk_is_paren_open(tmp))
      {
         tmp = chunk_get_next_nnl(chunk_skip_to_match(tmp));
      }
      fix_var_def(tmp);
   }

   /**
    * Change the paren pair after a function/macrofunc.
    * CT_PAREN_OPEN => CT_FPAREN_OPEN
    */
   if (pc->type == CT_MACRO_FUNC)
   {
      flag_parens(next, PCF_IN_FCN_CALL, CT_FPAREN_OPEN, CT_MACRO_FUNC, false);
   }

   if ((pc->type == CT_MACRO_OPEN) ||
       (pc->type == CT_MACRO_ELSE) ||
       (pc->type == CT_MACRO_CLOSE))
   {
      if (next->type == CT_PAREN_OPEN)
      {
         flag_parens(next, 0, CT_FPAREN_OPEN, pc->type, false);
      }
   }

   if ((pc->type == CT_DELETE) && (next->type == CT_TSQUARE))
   {
      next->parent_type = CT_DELETE;
   }

   /* Change CT_STAR to CT_PTR_TYPE or CT_ARITH or CT_DEREF */
   if (pc->type == CT_STAR)
   {
      if (chunk_is_paren_close(next) || (next->type == CT_COMMA))
      {
         pc->type = CT_PTR_TYPE;
      }
      else if ((fpd.lang_flags & LANG_OC) && (next->type == CT_STAR))
      {
         /* Change pointer-to-pointer types in OC_MSG_DECLs
          * from ARITH <===> DEREF to PTR_TYPE <===> PTR_TYPE */
         pc->type        = CT_PTR_TYPE;
         pc->parent_type = prev->parent_type;

         next->type        = CT_PTR_TYPE;
         next->parent_type = pc->parent_type;
      }
      else if ((prev->type == CT_SIZEOF) || (prev->type == CT_DELETE))
      {
         pc->type = CT_DEREF;
      }
      else if (((prev->type == CT_WORD) && chunk_ends_type(prev)) ||
               (prev->type == CT_DC_MEMBER) || (prev->type == CT_PTR_TYPE))
      {
         pc->type = CT_PTR_TYPE;
      }
      else if (next->type == CT_SQUARE_OPEN)
      {
         pc->type = CT_PTR_TYPE;
      }
      else
      {
         /* most PCF_PUNCTUATOR chunks except a paren close would make this
          * a deref. A paren close may end a cast or may be part of a macro fcn.
          */
         pc->type = ((prev->flags & PCF_PUNCTUATOR) &&
                     (!chunk_is_paren_close(prev) ||
                      (prev->parent_type == CT_MACRO_FUNC)) &&
                     (prev->type != CT_SQUARE_CLOSE) &&
                     (prev->type != CT_DC_MEMBER)) ? CT_DEREF : CT_ARITH;
      }
   }

   if (pc->type == CT_AMP)
   {
      if (prev->type == CT_DELETE)
      {
         pc->type = CT_ADDR;
      }
      else if (prev->type == CT_TYPE)
      {
         pc->type = CT_BYREF;
      }
      else
      {
         pc->type = CT_ARITH;
         if (prev->type == CT_WORD)
         {
            tmp = chunk_get_prev_nnl(prev);
            if ((tmp != NULL) &&
                (chunk_is_semicolon(tmp) ||
                 (tmp->type == CT_BRACE_OPEN) ||
                 (tmp->type == CT_QUALIFIER)))
            {
               prev->type   = CT_TYPE;
               pc->type     = CT_ADDR;
            }
         }
      }
   }

   if ((pc->type == CT_MINUS) ||
       (pc->type == CT_PLUS))
   {
      if ((prev->type == CT_POS) || (prev->type == CT_NEG))
      {
         pc->type = (pc->type == CT_MINUS) ? CT_NEG : CT_POS;
      }
      else if (prev->type == CT_OC_CLASS)
      {
         pc->type = (pc->type == CT_MINUS) ? CT_NEG : CT_POS;
      }
      else
      {
         pc->type = CT_ARITH;
      }
   }
}


/**
 * Change CT_INCDEC_AFTER + WORD to CT_INCDEC_BEFORE
 * Change number/word + CT_ADDR to CT_ARITH
 * Change number/word + CT_STAR to CT_ARITH
 * Change number/word + CT_NEG to CT_ARITH
 * Change word + ( to a CT_FUNCTION
 * Change struct/union/enum + CT_WORD => CT_TYPE
 * Force parens on return.
 *
 * TODO: This could be done earlier.
 *
 * Patterns detected:
 *   STRUCT/ENUM/UNION + WORD :: WORD => TYPE
 *   WORD + '('               :: WORD => FUNCTION
 */
void fix_symbols(fp_data& fpd)
{
   chunk_t *pc;
   chunk_t *next;
   chunk_t *prev;
   chunk_t dummy;

   mark_define_expressions(fpd);

   for (pc = chunk_get_head(fpd); pc != NULL; pc = chunk_get_next_nnl(pc))
   {
      if ((pc->type == CT_FUNC_WRAP) ||
          (pc->type == CT_TYPE_WRAP))
      {
         handle_wrap(fpd, pc);
      }

      if (pc->type == CT_ASSIGN)
      {
         mark_lvalue(pc);
      }
   }

   pc = chunk_get_head(fpd);
   if (chunk_is_newline(pc))
   {
      pc = chunk_get_next_nnl(pc);
   }
   while (pc != NULL)
   {
      prev = chunk_get_prev_nnl(pc, CNAV_PREPROC);
      if (prev == NULL)
      {
         prev = &dummy;
      }
      next = chunk_get_next_nnl(pc, CNAV_PREPROC);
      if (next == NULL)
      {
         next = &dummy;
      }
      do_symbol_check(fpd, prev, pc, next);
      pc = chunk_get_next_nnl(pc);
   }

   pawn_add_virtual_semicolons(fpd);

   /**
    * 2nd pass - handle variable definitions
    * REVISIT: We need function params marked to do this (?)
    */
   pc = chunk_get_head(fpd);
   int square_level = -1;
   while (pc != NULL)
   {
      /* Can't have a variable definition inside [ ] */
      if (square_level < 0)
      {
         if (pc->type == CT_SQUARE_OPEN)
         {
            square_level = pc->level;
         }
      }
      else
      {
         if (pc->level <= square_level)
         {
            square_level = -1;
         }
      }

      /**
       * A variable definition is possible after at the start of a statement
       * that starts with: QUALIFIER, TYPE, or WORD
       */
      if ((square_level < 0) &&
          ((pc->flags & PCF_STMT_START) != 0) &&
          ((pc->type == CT_QUALIFIER) ||
           (pc->type == CT_TYPE) ||
           (pc->type == CT_WORD)) &&
          (pc->parent_type != CT_ENUM) &&
          ((pc->flags & PCF_IN_ENUM) == 0))
      {
         pc = fix_var_def(pc);
      }
      else
      {
         pc = chunk_get_next_nnl(pc);
      }
   }
}


/* Just hit an assign. Go backwards until we hit an open brace/paren/square or
 * semicolon (TODO: other limiter?) and mark as a LValue.
 */
static void mark_lvalue(chunk_t *pc)
{
   chunk_t *prev;

   if ((pc->flags & PCF_IN_PREPROC) != 0)
   {
      return;
   }

   for (prev = chunk_get_prev_nnl(pc);
        prev != NULL;
        prev = chunk_get_prev_nnl(prev))
   {
      if ((prev->level < pc->level) ||
          (prev->type == CT_ASSIGN) ||
          (prev->type == CT_COMMA) ||
          (prev->type == CT_BOOL) ||
          chunk_is_semicolon(prev) ||
          chunk_is_str(prev, "(", 1) ||
          chunk_is_str(prev, "{", 1) ||
          chunk_is_str(prev, "[", 1) ||
          (prev->flags & PCF_IN_PREPROC))
      {
         break;
      }
      prev->flags |= PCF_LVALUE;
      if ((prev->level == pc->level) && chunk_is_str(prev, "&", 1))
      {
         make_type(prev);
      }
   }
}


/**
 * Changes the return type to type and set the parent.
 *
 * @param pc the last chunk of the return type
 * @param parent_type CT_NONE (no change) or the new parent type
 */
static void mark_function_return_type(chunk_t *the_type, chunk_t *pc, c_token_t parent_type)
{
   if (pc)
   {
      /* Step backwards from pc and mark the parent of the return type */
      LOG_FMT(LFCNR, "%s: (backwards) return type for '%s' @ %d:%d", __func__,
              the_type->text(), the_type->orig_line, the_type->orig_col);

      while (pc)
      {
         if ((!chunk_is_type(pc) &&
              (pc->type != CT_OPERATOR) &&
              (pc->type != CT_WORD) &&
              (pc->type != CT_ADDR)) ||
             ((pc->flags & PCF_IN_PREPROC) != 0))
         {
            break;
         }
         LOG_FMT(LFCNR, " [%s|%s]", pc->text(), get_token_name(pc->type));

         if (pc->type == CT_QUALIFIER)
         {
            if (chunk_is_str(pc, "extern", 6))
            {
               if (the_type->flags & PCF_VAR_DEF)
               {
                  the_type->flags &= ~PCF_VAR_DEF;
                  the_type->flags |= PCF_VAR_DECL;
               }
            }
            else if (chunk_is_str(pc, "static", 6))
            {
               the_type->flags |= PCF_STATIC;
            }
         }

         if (parent_type != CT_NONE)
         {
            pc->parent_type = parent_type;
         }
         make_type(pc);
         pc = chunk_get_prev_nnl(pc);
      }
      LOG_FMT(LFCNR, "\n");
   }
}


/**
 * Process a function type that is not in a typedef.
 * pc points to the first close paren.
 *
 * void (*func)(params);
 * const char * (*func)(params);
 * const char * (^func)(params);   -- Objective C
 *
 * @param pc   Points to the first closing paren
 * @return whether a function type was processed
 */
static bool mark_function_type(fp_data& fpd, chunk_t *pc)
{
   LOG_FMT(LFTYPE, "%s: [%s] %s @ %d:%d\n",
           __func__, get_token_name(pc->type), pc->str.c_str(),
           pc->orig_line, pc->orig_col);

   int     star_count = 0;
   int     word_count = 0;
   chunk_t *ptrcnk    = NULL;
   chunk_t *varcnk    = NULL;
   chunk_t *tmp;
   chunk_t *apo;
   chunk_t *apc;
   chunk_t *aft;
   bool    anon = false;
   c_token_t pt, ptp;

   /* Scan backwards across the name, which can only be a word and single star */
   varcnk = chunk_get_prev_nnl(pc);
   if (!chunk_is_word(varcnk))
   {
      if ((fpd.lang_flags & LANG_OC) && chunk_is_str(varcnk, "^", 1) &&
          chunk_is_paren_open(chunk_get_prev_nnl(varcnk)))
      {
         /* anonymous ObjC block type -- RTYPE (^)(ARGS) */
         anon = true;
      }
      else
      {
         LOG_FMT(LFTYPE, "%s: not a word '%s' [%s] @ %d:%d\n",
                 __func__, varcnk->text(), get_token_name(varcnk->type),
                 varcnk->orig_line, varcnk->orig_col);
         goto nogo_exit;
      }
   }

   apo = chunk_get_next_nnl(pc);
   apc = chunk_skip_to_match(apo);
   if (!chunk_is_paren_open(apo) || ((apc = chunk_skip_to_match(apo)) == NULL))
   {
      LOG_FMT(LFTYPE, "%s: not followed by parens\n", __func__);
      goto nogo_exit;
   }
   aft = chunk_get_next_nnl(apc);
   if (chunk_is_token(aft, CT_BRACE_OPEN))
   {
      pt  = CT_FUNC_DEF;
   }
   else if (chunk_is_token(aft, CT_SEMICOLON) ||
            chunk_is_token(aft, CT_ASSIGN))
   {
      pt  = CT_FUNC_PROTO;
   }
   else
   {
      LOG_FMT(LFTYPE, "%s: not followed by '{' or ';'\n", __func__);
      goto nogo_exit;
   }
   ptp = (pc->flags & PCF_IN_TYPEDEF) ? CT_FUNC_TYPE : CT_FUNC_VAR;

   tmp = pc;
   while ((tmp = chunk_get_prev_nnl(tmp)) != NULL)
   {
      LOG_FMT(LFTYPE, " -- [%s] %s on line %d, col %d",
              get_token_name(tmp->type), tmp->str.c_str(),
              tmp->orig_line, tmp->orig_col);

      if (chunk_is_star(tmp) || chunk_is_token(tmp, CT_PTR_TYPE) ||
          chunk_is_token(tmp, CT_CARET))
      {
         star_count++;
         ptrcnk = tmp;
         LOG_FMT(LFTYPE, " -- PTR_TYPE\n");
      }
      else if (chunk_is_word(tmp) ||
               (tmp->type == CT_WORD) ||
               (tmp->type == CT_TYPE))
      {
         word_count++;
         LOG_FMT(LFTYPE, " -- TYPE(%s)\n", tmp->text());
      }
      else if (tmp->type == CT_DC_MEMBER)
      {
         word_count = 0;
         LOG_FMT(LFTYPE, " -- :: reset word_count\n");
      }
      else if (chunk_is_str(tmp, "(", 1))
      {
         LOG_FMT(LFTYPE, " -- open paren (break)\n");
         break;
      }
      else
      {
         LOG_FMT(LFTYPE, " --  unexpected token [%s] %s on line %d, col %d\n",
                 get_token_name(tmp->type), tmp->str.c_str(),
                 tmp->orig_line, tmp->orig_col);
         goto nogo_exit;
      }
   }

   if ((star_count > 1) ||
       (word_count > 1) ||
       ((star_count + word_count) == 0))
   {
      LOG_FMT(LFTYPE, "%s: bad counts word:%d, star:%d\n", __func__,
              word_count, star_count);
      goto nogo_exit;
   }

   /* make sure what appears before the first open paren can be a return type */
   if (!chunk_ends_type(chunk_get_prev_nnl(tmp)))
   {
      goto nogo_exit;
   }

   if (ptrcnk)
   {
      ptrcnk->type = CT_PTR_TYPE;
   }
   if (!anon)
   {
      if (pc->flags & PCF_IN_TYPEDEF)
      {
         varcnk->type = CT_FUNC_TYPE;
      }
      else
      {
         varcnk->type   = CT_FUNC_VAR;
         varcnk->flags |= PCF_VAR_DEF;
      }
   }
   pc->type        = CT_TPAREN_CLOSE;
   pc->parent_type = ptp;

   apo->type        = CT_FPAREN_OPEN;
   apo->parent_type = pt;
   apc->type        = CT_FPAREN_CLOSE;
   apc->parent_type = pt;
   fix_fcn_def_params(fpd, apo);

   if (chunk_is_semicolon(aft))
   {
      aft->parent_type = (aft->flags & PCF_IN_TYPEDEF) ? CT_TYPEDEF : CT_FUNC_VAR;
   }
   else if (chunk_is_token(aft, CT_BRACE_OPEN))
   {
      flag_parens(aft, 0, CT_NONE, pt, false);
   }

   /* Step backwards to the previous open paren and mark everything a
    */
   tmp = pc;
   while ((tmp = chunk_get_prev_nnl(tmp)) != NULL)
   {
      LOG_FMT(LFTYPE, " ++ [%s] %s on line %d, col %d\n",
              get_token_name(tmp->type), tmp->str.c_str(),
              tmp->orig_line, tmp->orig_col);

      if (*tmp->str.data() == '(')
      {
         if ((pc->flags & PCF_IN_TYPEDEF) == 0)
         {
            tmp->flags      |= PCF_VAR_DEF;
         }
         tmp->type        = CT_TPAREN_OPEN;
         tmp->parent_type = ptp;

         tmp = chunk_get_prev_nnl(tmp);
         if (tmp != NULL)
         {
            if ((tmp->type == CT_FUNCTION) ||
                (tmp->type == CT_FUNC_CALL) ||
                (tmp->type == CT_FUNC_CALL_USER) ||
                (tmp->type == CT_FUNC_DEF) ||
                (tmp->type == CT_FUNC_PROTO))
            {
               tmp->type   = CT_TYPE;
               tmp->flags &= ~PCF_VAR_DEF;
            }
         }
         mark_function_return_type(varcnk, tmp, ptp);
         break;
      }
   }
   return true;

nogo_exit:
   tmp = chunk_get_next_nnl(pc);
   if (chunk_is_paren_open(tmp))
   {
      LOG_FMT(LFTYPE, "%s:%d setting FUNC_CALL on %d:%d\n", __func__, __LINE__,
              tmp->orig_line, tmp->orig_col);
      flag_parens(tmp, 0, CT_FPAREN_OPEN, CT_FUNC_CALL, false);
   }
   return false;
}


static bool is_ucase_str(const char *str, int len)
{
   while (len-- > 0)
   {
      if (toupper((unsigned char) *str) != *str)
      {
         return(false);
      }
      str++;
   }
   return(true);
}


static bool is_oc_block(chunk_t *pc)
{
   return((pc != NULL) &&
          ((pc->parent_type == CT_OC_BLOCK_TYPE) ||
           (pc->parent_type == CT_OC_BLOCK_EXPR) ||
           (pc->parent_type == CT_OC_BLOCK_ARG) ||
           (pc->parent_type == CT_OC_BLOCK) ||
           (pc->type == CT_OC_BLOCK_CARET) ||
           (pc->next && pc->next->type == CT_OC_BLOCK_CARET) ||
           (pc->prev && pc->prev->type == CT_OC_BLOCK_CARET)));
}


/**
 * Checks to see if the current paren is part of a cast.
 * We already verified that this doesn't follow function, TYPE, IF, FOR,
 * SWITCH, or WHILE and is followed by WORD, TYPE, STRUCT, ENUM, or UNION.
 *
 * @param start   Pointer to the open paren
 */
static void fix_casts(chunk_t *start)
{
   chunk_t    *pc;
   chunk_t    *prev;
   chunk_t    *first;
   chunk_t    *after;
   chunk_t    *last = NULL;
   chunk_t    *paren_close;
   const char *verb       = "likely";
   const char *detail     = "";
   int        count       = 0;
   int        word_count  = 0;
   int        word_consec = 0;
   bool       nope;
   bool       doubtful_cast = false;


   LOG_FMT(LCASTS, "%s:line %d, col %d:", __func__, start->orig_line, start->orig_col);

   prev = chunk_get_prev_nnl(start);
   if ((prev != NULL) && (prev->type == CT_PP_DEFINED))
   {
      LOG_FMT(LCASTS, " -- not a cast - after defined\n");
      return;
   }

   /* Make sure there is only WORD, TYPE, and '*' before the close paren */
   pc    = chunk_get_next_nnl(start);
   first = pc;
   while ((pc != NULL) && (chunk_is_type(pc) ||
                           (pc->type == CT_WORD) ||
                           (pc->type == CT_QUALIFIER) ||
                           (pc->type == CT_DC_MEMBER) ||
                           (pc->type == CT_STAR) ||
                           (pc->type == CT_AMP)))
   {
      LOG_FMT(LCASTS, " [%s]", get_token_name(pc->type));

      if (pc->type == CT_WORD)
      {
         word_count++;
         word_consec++;
      }
      else if (pc->type == CT_DC_MEMBER)
      {
         word_count--;
      }
      else
      {
         word_consec = 0;
      }

      last = pc;
      pc   = chunk_get_next_nnl(pc);
      count++;
   }

   if ((pc == NULL) || (pc->type != CT_PAREN_CLOSE) || (prev->type == CT_OC_CLASS))
   {
      LOG_FMT(LCASTS, " -- not a cast, hit [%s]\n",
              pc == NULL ? "NULL"  : get_token_name(pc->type));
      return;
   }

   if (word_count > 1)
   {
      LOG_FMT(LCASTS, " -- too many words: %d\n", word_count);
      return;
   }
   paren_close = pc;

   /* If last is a type or star, we have a cast for sure */
   if ((last->type == CT_STAR) ||
       (last->type == CT_PTR_TYPE) ||
       (last->type == CT_TYPE))
   {
      verb = "for sure";
   }
   else if (count == 1)
   {
      /**
       * We are on a potential cast of the form "(word)".
       * We don't know if the word is a type. So lets guess based on some
       * simple rules:
       *  - if all caps, likely a type
       *  - if it ends in _t, likely a type
       */
      verb = "guessed";
      if ((last->len() > 3) &&
          (last->str[last->len() - 2] == '_') &&
          (last->str[last->len() - 1] == 't'))
      {
         detail = " -- '_t'";
      }
      else if (is_ucase_str(last->text(), last->len()))
      {
         detail = " -- upper case";
      }
      else
      {
         /* If we can't tell for sure whether this is a cast, decide against it */
         detail        = " -- mixed case";
         doubtful_cast = true;
      }

      /**
       * If the next item is a * or &, the next item after that can't be a
       * number or string.
       *
       * If the next item is a +, the next item has to be a number.
       *
       * If the next item is a -, the next item can't be a string.
       *
       * For this to be a cast, the close paren must be followed by:
       *  - constant (number or string)
       *  - paren open
       *  - word
       *
       * Find the next non-open paren item.
       */
      pc    = chunk_get_next_nnl(paren_close);
      after = pc;
      do
      {
         after = chunk_get_next_nnl(after);
      } while ((after != NULL) && (after->type == CT_PAREN_OPEN));

      if (after == NULL)
      {
         LOG_FMT(LCASTS, " -- not a cast - hit NULL\n");
         return;
      }

      nope = false;
      if (chunk_is_star(pc) || chunk_is_addr(pc))
      {
         /* star (*) and addr (&) are ambiguous */
         if ((after->type == CT_NUMBER_FP) ||
             (after->type == CT_NUMBER) ||
             (after->type == CT_STRING) ||
             doubtful_cast)
         {
            nope = true;
         }
      }
      else if (pc->type == CT_MINUS)
      {
         /* (UINT8)-1 or (foo)-1 or (FOO)-'a' */
         if ((after->type == CT_STRING) || doubtful_cast)
         {
            nope = true;
         }
      }
      else if (pc->type == CT_PLUS)
      {
         /* (UINT8)+1 or (foo)+1 */
         if (((after->type != CT_NUMBER) &&
              (after->type != CT_NUMBER_FP)) || doubtful_cast)
         {
            nope = true;
         }
      }
      else if ((pc->type != CT_NUMBER_FP) &&
               (pc->type != CT_NUMBER) &&
               (pc->type != CT_WORD) &&
               (pc->type != CT_TYPE) &&
               (pc->type != CT_PAREN_OPEN) &&
               (pc->type != CT_STRING) &&
               (pc->type != CT_SIZEOF) &&
               (pc->type != CT_FUNC_CALL) &&
               (pc->type != CT_FUNC_CALL_USER) &&
               (pc->type != CT_FUNCTION) &&
               (pc->type != CT_BRACE_OPEN))
      {
         LOG_FMT(LCASTS, " -- not a cast - followed by '%s' %s\n",
                 pc->str.c_str(), get_token_name(pc->type));
         return;
      }

      if (nope)
      {
         LOG_FMT(LCASTS, " -- not a cast - '%s' followed by %s\n",
                 pc->str.c_str(), get_token_name(after->type));
         return;
      }
   }

   /* if the 'cast' is followed by a semicolon, comma or close paren, it isn't */
   pc = chunk_get_next_nnl(paren_close);
   if (chunk_is_semicolon(pc) || chunk_is_token(pc, CT_COMMA) || chunk_is_paren_close(pc))
   {
      LOG_FMT(LCASTS, " -- not a cast - followed by %s\n", get_token_name(pc->type));
      return;
   }

   start->parent_type       = CT_C_CAST;
   paren_close->parent_type = CT_C_CAST;

   LOG_FMT(LCASTS, " -- %s c-cast: (", verb);

   for (pc = first; pc != paren_close; pc = chunk_get_next_nnl(pc))
   {
      pc->parent_type = CT_C_CAST;
      make_type(pc);
      LOG_FMT(LCASTS, " %s", pc->str.c_str());
   }
   LOG_FMT(LCASTS, " )%s\n", detail);

   /* Mark the next item as an expression start */
   pc = chunk_get_next_nnl(paren_close);
   if (pc != NULL)
   {
      pc->flags |= PCF_EXPR_START;
      if (chunk_is_opening_brace(pc))
      {
         set_paren_parent(pc, start->parent_type);
      }
   }
}


/**
 * CT_TYPE_CAST follows this pattern:
 * dynamic_cast<...>(...)
 *
 * Mark everything between the <> as a type and set the paren parent
 */
static void fix_type_cast(chunk_t *start)
{
   chunk_t *pc;

   pc = chunk_get_next_nnl(start);
   if ((pc == NULL) || (pc->type != CT_ANGLE_OPEN))
   {
      return;
   }

   while (((pc = chunk_get_next_nnl(pc)) != NULL) &&
          (pc->level >= start->level))
   {
      if ((pc->level == start->level) && (pc->type == CT_ANGLE_CLOSE))
      {
         pc = chunk_get_next_nnl(pc);
         if (chunk_is_str(pc, "(", 1))
         {
            set_paren_parent(pc, CT_TYPE_CAST);
         }
         return;
      }
      make_type(pc);
   }
}


/**
 * We are on an enum/struct/union tag
 * If there is a {...} and words before the ';', then they are variables.
 *
 * tag { ... } [*] word [, [*]word] ;
 * tag [word/type] { ... } [*] word [, [*]word] ;
 * enum [word/type [: int_type]] { ... } [*] word [, [*]word] ;
 * tag [word/type] [word]; -- this gets caught later.
 * fcn(tag [word/type] [word])
 * a = (tag [word/type] [*])&b;
 *
 * REVISIT: should this be consolidated with the typedef code?
 */
static void fix_enum_struct_union(fp_data& fpd, chunk_t *pc)
{
   chunk_t *next;
   chunk_t *prev        = NULL;
   int     flags        = PCF_VAR_DEF;
   int     in_fcn_paren = pc->flags & PCF_IN_FCN_DEF;

   /* Make sure this wasn't a cast */
   if (pc->parent_type == CT_C_CAST)
   {
      return;
   }

   /* the next item is either a type or open brace */
   next = chunk_get_next_nnl(pc);
   if (next && (next->type == CT_ENUM_CLASS))
   {
      next = chunk_get_next_nnl(next);
   }
   if (next && (next->type == CT_TYPE))
   {
      next->parent_type = pc->type;
      prev = next;
      next = chunk_get_next_nnl(next);

      /* next up is either a colon, open brace, or open paren (pawn) */
      if (!next)
      {
         return;
      }
      else if (((fpd.lang_flags & LANG_PAWN) != 0) &&
               (next->type == CT_PAREN_OPEN))
      {
         next = set_paren_parent(next, CT_ENUM);
      }
      else if ((pc->type == CT_ENUM) && (next->type == CT_COLON))
      {
         /* enum TYPE : INT_TYPE { */
         next = chunk_get_next_nnl(next);
         if (next)
         {
            make_type(next);
            next = chunk_get_next_nnl(next);
         }
      }
   }
   if (next && (next->type == CT_BRACE_OPEN))
   {
      flag_parens(next, (pc->type == CT_ENUM) ? PCF_IN_ENUM : PCF_IN_STRUCT,
                  CT_NONE, CT_NONE, false);

      if ((pc->type == CT_UNION) || (pc->type == CT_STRUCT))
      {
         mark_struct_union_body(next);
      }

      /* Skip to the closing brace */
      next->parent_type = pc->type;
      next   = chunk_get_next_type(next, CT_BRACE_CLOSE, pc->level);
      flags |= PCF_VAR_INLINE;
      if (next != NULL)
      {
         next->parent_type = pc->type;
         next = chunk_get_next_nnl(next);
      }
      if (prev != NULL)
      {
         prev->flags |= PCF_DEF;
      }
      prev = NULL;
   }
   else if (prev != NULL)
   {
      if (!chunk_is_semicolon(next))
      {
         prev->flags |= PCF_REF;
      }
      else
      {
         prev->flags |= PCF_PROTO;
      }
   }

   if ((next == NULL) || (next->type == CT_PAREN_CLOSE))
   {
      return;
   }

   if (!chunk_is_semicolon(next))
   {
      /* Pawn does not require a semicolon after an enum */
      if (fpd.lang_flags & LANG_PAWN)
      {
         return;
      }

      /* D does not require a semicolon after an enum, but we add one to make
       * other code happy.
       */
      if (fpd.lang_flags & LANG_D)
      {
         next = pawn_add_vsemi_after(fpd, chunk_get_prev_nnl(next));
      }
   }

   /* We are either pointing to a ';' or a variable */
   while ((next != NULL) && !chunk_is_semicolon(next) &&
          (next->type != CT_ASSIGN) &&
          ((in_fcn_paren ^ (next->flags & PCF_IN_FCN_DEF)) == 0))
   {
      if (next->level == pc->level)
      {
         if (next->type == CT_WORD)
         {
            next->flags |= flags;
         }

         if (next->type == CT_STAR)
         {
            next->type = CT_PTR_TYPE;
         }

         /* If we hit a comma in a function param, we are done */
         if (((next->type == CT_COMMA) ||
              (next->type == CT_FPAREN_CLOSE)) &&
             ((next->flags & (PCF_IN_FCN_DEF | PCF_IN_FCN_CALL)) != 0))
         {
            return;
         }
      }

      next = chunk_get_next_nnl(next);
   }

   if (next && !prev && (next->type == CT_SEMICOLON) && (next->parent_type == CT_NONE))
   {
      next->parent_type = pc->type;
   }
}


/**
 * We are on a typedef.
 * If the next word is not enum/union/struct, then the last word before the
 * next ',' or ';' or '__attribute__' is a type.
 *
 * typedef [type...] [*] type [, [*]type] ;
 * typedef <return type>([*]func)(params);
 * typedef <return type>func(params);
 * typedef <enum/struct/union> [type] [*] type [, [*]type] ;
 * typedef <enum/struct/union> [type] { ... } [*] type [, [*]type] ;
 */
static void fix_typedef(fp_data& fpd, chunk_t *start)
{
   chunk_t   *next;
   chunk_t   *the_type = NULL;
   chunk_t   *last_op = NULL;
   c_token_t tag;

   LOG_FMT(LTYPEDEF, "%s: typedef @ %d:%d\n", __func__, start->orig_line, start->orig_col);

   /* Mark everything in the typedef and scan for ")(", which makes it a
    * function type
    */
   next = start;
   while (((next = chunk_get_next_nnl(next, CNAV_PREPROC)) != NULL) &&
          (next->level >= start->level))
   {
      next->flags |= PCF_IN_TYPEDEF;
      if (start->level == next->level)
      {
         if (chunk_is_semicolon(next))
         {
            next->parent_type = CT_TYPEDEF;
            break;
         }
         if (next->type == CT_ATTRIBUTE)
         {
            break;
         }
         if ((fpd.lang_flags & LANG_D) && (next->type == CT_ASSIGN))
         {
            next->parent_type = CT_TYPEDEF;
            break;
         }
         make_type(next);
         if (next->type == CT_TYPE)
         {
            the_type = next;
         }
         next->flags &= ~PCF_VAR_DEF;
         if (*next->str.data() == '(')
         {
            last_op = next;
         }
      }
   }

   if (last_op)
   {
      flag_parens(last_op, 0, CT_FPAREN_OPEN, CT_TYPEDEF, false);
      fix_fcn_def_params(fpd, last_op);

      the_type = chunk_get_prev_nnl(last_op, CNAV_PREPROC);
      if (chunk_is_paren_close(the_type))
      {
         mark_function_type(fpd, the_type);
         the_type = chunk_get_prev_nnl(the_type, CNAV_PREPROC);
      }
      else
      {
         /* must be: "typedef <return type>func(params);" */
         the_type->type = CT_FUNC_TYPE;
      }
      the_type->parent_type = CT_TYPEDEF;

      LOG_FMT(LTYPEDEF, "%s: fcn typedef [%s] on line %d\n", __func__,
              the_type->text(), the_type->orig_line);

      /* already did everything we need to do */
      return;
   }

   /**
    * Skip over enum/struct/union stuff, as we know it isn't a return type
    * for a function type
    */
   next = chunk_get_next_nnl(start, CNAV_PREPROC);
   if ((next->type != CT_ENUM) &&
       (next->type != CT_STRUCT) &&
       (next->type != CT_UNION))
   {
      if (the_type != NULL)
      {
         /* We have just a regular typedef */
         LOG_FMT(LTYPEDEF, "%s: regular typedef [%s] on line %d\n", __func__,
                 the_type->str.c_str(), the_type->orig_line);
         the_type->parent_type = CT_TYPEDEF;
      }
      return;
   }

   /* We have a struct/union/enum type, set the parent */
   tag = next->type;

   /* the next item should be either a type or { */
   next = chunk_get_next_nnl(next, CNAV_PREPROC);
   if (next->type == CT_TYPE)
   {
      next = chunk_get_next_nnl(next, CNAV_PREPROC);
   }
   if (next->type == CT_BRACE_OPEN)
   {
      next->parent_type = tag;
      /* Skip to the closing brace */
      next = chunk_get_next_type(next, CT_BRACE_CLOSE, next->level, CNAV_PREPROC);
      if (next != NULL)
      {
         next->parent_type = tag;
      }
   }

   if (the_type != NULL)
   {
      LOG_FMT(LTYPEDEF, "%s: %s typedef [%s] on line %d\n",
              __func__, get_token_name(tag), the_type->str.c_str(), the_type->orig_line);
      the_type->parent_type = CT_TYPEDEF;
      if (tag == CT_STRUCT)
         the_type->flags |= PCF_TYPEDEF_STRUCT;
      else if (tag == CT_UNION)
         the_type->flags |= PCF_TYPEDEF_UNION;
      else if (tag == CT_ENUM)
         the_type->flags |= PCF_TYPEDEF_ENUM;
   }
}


/**
 * Examines the whole file and changes CT_COLON to
 * CT_Q_COLON, CT_LABEL_COLON, or CT_CASE_COLON.
 * It also changes the CT_WORD before CT_LABEL_COLON into CT_LABEL.
 */
void combine_labels(fp_data& fpd)
{
   chunk_t *cur;
   chunk_t *prev;
   chunk_t *next;
   chunk_t *tmp;
   int     question_count = 0;
   bool    hit_case       = false;
   bool    hit_class      = false;

   prev = chunk_get_head(fpd);
   cur  = chunk_get_next(prev);
   next = chunk_get_next(cur);

   /* unlikely that the file will start with a label... */
   while (next != NULL)
   {
      if (!(next->flags & PCF_IN_OC_MSG) && /* filter OC case of [self class] msg send */
          ((next->type == CT_CLASS) ||
           (next->type == CT_OC_CLASS) ||
           (next->type == CT_TEMPLATE)))
      {
         hit_class = true;
      }
      if (chunk_is_semicolon(next) || (next->type == CT_BRACE_OPEN))
      {
         hit_class = false;
      }
      if (next->type == CT_QUESTION)
      {
         question_count++;
      }
      else if (next->type == CT_CASE)
      {
         if (cur->type == CT_GOTO)
         {
            /* handle "goto case x;" */
            next->type = CT_QUALIFIER;
         }
         else
         {
            hit_case = true;
         }
      }
      else if (next->type == CT_COLON)
      {
         if (cur->type == CT_DEFAULT)
         {
            cur->type = CT_CASE;
            hit_case  = true;
         }
         if (question_count > 0)
         {
            next->type = CT_COND_COLON;
            question_count--;
         }
         else if (hit_case)
         {
            hit_case   = false;
            next->type = CT_CASE_COLON;
            tmp        = chunk_get_next_nnl(next);
            if ((tmp != NULL) && (tmp->type == CT_BRACE_OPEN))
            {
               tmp->parent_type = CT_CASE;
               tmp = chunk_get_next_type(tmp, CT_BRACE_CLOSE, tmp->level);
               if (tmp != NULL)
               {
                  tmp->parent_type = CT_CASE;
               }
            }
         }
         else
         {
            chunk_t *nextprev = chunk_get_prev_nnl(next);

            if ((fpd.lang_flags & LANG_PAWN) != 0)
            {
               if ((cur->type == CT_WORD) ||
                   (cur->type == CT_BRACE_CLOSE))
               {
                  c_token_t new_type = CT_TAG;

                  tmp = chunk_get_next(next);
                  if (chunk_is_newline(prev) && chunk_is_newline(tmp))
                  {
                     new_type   = CT_LABEL;
                     next->type = CT_LABEL_COLON;
                  }
                  else
                  {
                     next->type = CT_TAG_COLON;
                  }
                  if (cur->type == CT_WORD)
                  {
                     cur->type = new_type;
                  }
               }
            }
            else if (next->flags & PCF_IN_ARRAY_ASSIGN)
            {
               next->type = CT_D_ARRAY_COLON;
            }
            else if (next->flags & PCF_IN_FOR)
            {
               next->type = CT_FOR_COLON;
            }
            else if (next->flags & PCF_OC_BOXED)
            {
               next->type = CT_OC_DICT_COLON;
            }
            else if (cur->type == CT_WORD)
            {
               tmp = chunk_get_next(next, CNAV_PREPROC);
               if (chunk_is_newline(prev) && ((tmp == NULL) || (tmp->type != CT_NUMBER)))
               {
                  cur->type  = CT_LABEL;
                  next->type = CT_LABEL_COLON;
               }
               else if (next->flags & PCF_IN_FCN_CALL)
               {
                  /* Must be a macro thingy, assume some sort of label */
                  next->type = CT_LABEL_COLON;
               }
               else
               {
                  next->type = CT_BIT_COLON;

                  tmp = chunk_get_next(next);
                  while ((tmp = chunk_get_next(tmp)) != NULL)
                  {
                     if (tmp->type == CT_SEMICOLON)
                     {
                        break;
                     }
                     if (tmp->type == CT_COLON)
                     {
                        tmp->type = CT_BIT_COLON;
                     }
                  }
               }
            }
            else if (nextprev->type == CT_FPAREN_CLOSE)
            {
               /* it's a class colon */
               next->type = CT_CLASS_COLON;
            }
            else if (next->level > next->brace_level)
            {
               /* ignore it, as it is inside a paren */
            }
            else if (cur->type == CT_TYPE)
            {
               next->type = CT_BIT_COLON;
            }
            else if ((cur->type == CT_ENUM) ||
                     (cur->type == CT_PRIVATE) ||
                     (cur->type == CT_QUALIFIER) ||
                     (cur->parent_type == CT_ALIGN))
            {
               /* ignore it - bit field, align or public/private, etc */
            }
            else if ((cur->type == CT_ANGLE_CLOSE) || hit_class)
            {
               /* ignore it - template thingy */
            }
            else if (cur->parent_type == CT_SQL_EXEC)
            {
               /* ignore it - SQL variable name */
            }
            else if (next->parent_type == CT_ASSERT)
            {
               /* ignore it - Java assert thing */
            }
            else
            {
               tmp = chunk_get_next_nnl(next);
               if ((tmp != NULL) && ((tmp->type == CT_BASE) ||
                                     (tmp->type == CT_THIS)))
               {
                  /* ignore it, as it is a C# base thingy */
               }
               else
               {
                  LOG_FMT(LWARN, "%s:%d unexpected colon in col %d n-parent=%s c-parent=%s l=%d bl=%d\n",
                          fpd.filename, next->orig_line, next->orig_col,
                          get_token_name(next->parent_type),
                          get_token_name(cur->parent_type),
                          next->level, next->brace_level);
               }
            }
         }
      }
      prev = cur;
      cur  = next;
      next = chunk_get_next(cur);
   }
}


static void mark_variable_stack(ChunkStack& cs, log_sev_t sev)
{
   chunk_t *var_name;
   chunk_t *word_type;
   int     word_cnt = 0;

   /* throw out the last word and mark the rest */
   var_name = cs.Pop_Back();
   if (var_name && var_name->prev->type == CT_DC_MEMBER)
   {
     cs.Push_Back(var_name);
   }

   if (var_name != NULL)
   {
      LOG_FMT(LFCNP, "%s: parameter on line %d :",
              __func__, var_name->orig_line);

      while ((word_type = cs.Pop_Back()) != NULL)
      {
         if ((word_type->type == CT_WORD) || (word_type->type == CT_TYPE))
         {
            LOG_FMT(LFCNP, " <%s>", word_type->str.c_str());

            word_type->type   = CT_TYPE;
            word_type->flags |= PCF_VAR_TYPE;
         }
         word_cnt++;
      }

      if (var_name->type == CT_WORD)
      {
         if (word_cnt)
         {
            LOG_FMT(LFCNP, " [%s]\n", var_name->str.c_str());
            var_name->flags |= PCF_VAR_DEF;
         }
         else
         {
            LOG_FMT(LFCNP, " <%s>\n", var_name->str.c_str());
            var_name->type   = CT_TYPE;
            var_name->flags |= PCF_VAR_TYPE;
         }
      }
   }
}


/**
 * Simply change any STAR to PTR_TYPE and WORD to TYPE
 *
 * @param start points to the open paren
 */
static void fix_fcn_def_params(fp_data& fpd, chunk_t *start)
{
   LOG_FMT(LFCNP, "%s: %s [%s] on line %d, level %d\n",
           __func__, start->str.c_str(), get_token_name(start->type), start->orig_line, start->level);

   while ((start != NULL) && !chunk_is_paren_open(start))
   {
      start = chunk_get_next_nnl(start);
   }

   assert((start->len() == 1) && (start->str[0] == '('));

   ChunkStack cs;

   int level = start->level + 1;

   chunk_t *pc = start;
   while ((pc = chunk_get_next_nnl(pc)) != NULL)
   {
      if (((start->len() == 1) && (start->str[0] == ')')) ||
          (pc->level < level))
      {
         LOG_FMT(LFCNP, "%s: bailed on %s on line %d\n", __func__, pc->str.c_str(), pc->orig_line);
         break;
      }

      LOG_FMT(LFCNP, "%s: %s %s on line %d, level %d\n", __func__,
              (pc->level > level) ? "skipping" : "looking at",
              pc->str.c_str(), pc->orig_line, pc->level);

      if (pc->level > level)
      {
         continue;
      }
      if (chunk_is_star(pc))
      {
         pc->type = CT_PTR_TYPE;
         cs.Push_Back(pc);
      }
      else if ((pc->type == CT_AMP) ||
               ((fpd.lang_flags & LANG_CPP) && chunk_is_str(pc, "&&", 2)))
      {
         pc->type = CT_BYREF;
         cs.Push_Back(pc);
      }
      else if (pc->type == CT_TYPE_WRAP)
      {
         cs.Push_Back(pc);
      }
      else if ((pc->type == CT_WORD) || (pc->type == CT_TYPE))
      {
         cs.Push_Back(pc);
      }
      else if ((pc->type == CT_COMMA) || (pc->type == CT_ASSIGN))
      {
         mark_variable_stack(cs, LFCNP);
         if (pc->type == CT_ASSIGN)
         {
            /* Mark assignment for default param spacing */
            pc->parent_type = CT_FUNC_PROTO;
         }
      }
   }
   mark_variable_stack(cs, LFCNP);
}


/**
 * Skips to the start of the next statement.
 */
static chunk_t *skip_to_next_statement(chunk_t *pc)
{
   while ((pc != NULL) && !chunk_is_semicolon(pc) &&
          (pc->type != CT_BRACE_OPEN) &&
          (pc->type != CT_BRACE_CLOSE))
   {
      pc = chunk_get_next_nnl(pc);
   }
   return(pc);
}


/**
 * We are on the start of a sequence that could be a var def
 *  - FPAREN_OPEN (parent == CT_FOR)
 *  - BRACE_OPEN
 *  - SEMICOLON
 *
 */
static chunk_t *fix_var_def(chunk_t *start)
{
   chunk_t    *pc = start;
   chunk_t    *end;
   chunk_t    *tmp_pc;
   ChunkStack cs;
   int        idx, ref_idx;
   UINT64     flags = PCF_VAR_DEF;

   LOG_FMT(LFVD, "%s: start[%d:%d]", __func__, pc->orig_line, pc->orig_col);

   /* Scan for words and types and stars oh my! */
   while ((pc != NULL) &&
          ((pc->type == CT_TYPE) ||
           (pc->type == CT_WORD) ||
           (pc->type == CT_QUALIFIER) ||
           (pc->type == CT_DC_MEMBER) ||
           (pc->type == CT_MEMBER) ||
           chunk_is_addr(pc) ||
           chunk_is_star(pc)))
   {
      LOG_FMT(LFVD, " %s[%s]", pc->str.c_str(), get_token_name(pc->type));
      cs.Push_Back(pc);

      if (pc->type == CT_QUALIFIER)
      {
         if (chunk_is_str(pc, "extern", 6))
         {
            flags &= ~PCF_VAR_DEF;
            flags |= PCF_VAR_DECL;
         }
         else if (chunk_is_str(pc, "static", 6))
         {
            flags |= PCF_STATIC;
         }
      }

      pc = chunk_get_next_nnl(pc);

      /* Skip templates and attributes */
      pc = skip_template_next(pc);
      pc = skip_attribute_next(pc);
   }
   end = pc;

   LOG_FMT(LFVD, " end=[%s]\n", (end != NULL) ? get_token_name(end->type) : "NULL");

   if (end == NULL)
   {
      return(NULL);
   }

   /* Function defs are handled elsewhere */
   if ((cs.Len() <= 1) ||
       (end->type == CT_FUNC_DEF) ||
       (end->type == CT_FUNC_PROTO) ||
       (end->type == CT_FUNC_CLASS) ||
       (end->type == CT_OPERATOR))
   {
      return(skip_to_next_statement(end));
   }

   /* ref_idx points to the alignable part of the var def */
   ref_idx = cs.Len() - 1;

   /* Check for the '::' stuff: "char *Engine::name" */
   if ((cs.Len() >= 3) &&
       ((cs.Get(cs.Len() - 2)->m_pc->type == CT_MEMBER) ||
        (cs.Get(cs.Len() - 2)->m_pc->type == CT_DC_MEMBER)))
   {
      idx = cs.Len() - 2;
      while (idx > 0)
      {
         chunk_t *tmp_pc1 = cs.Get(idx)->m_pc;
         if ((tmp_pc1->type != CT_DC_MEMBER) &&
             (tmp_pc1->type != CT_MEMBER))
         {
            break;
         }
         idx--;
         chunk_t *tmp_pc2 = cs.Get(idx)->m_pc;
         if ((tmp_pc2->type != CT_WORD) &&
             (tmp_pc2->type != CT_TYPE))
         {
            break;
         }
         if (tmp_pc1->type == CT_DC_MEMBER)
         {
            LOG_FMT(LFVD, " make_type %s[%s]\n", tmp_pc2->str.c_str(), get_token_name(tmp_pc2->type));
            make_type(tmp_pc2);
         }
         idx--;
      }
      ref_idx = idx + 1;
   }
   tmp_pc = cs.Get(ref_idx)->m_pc;
   LOG_FMT(LFVD, " ref_idx(%d) => %s\n", ref_idx, tmp_pc->str.c_str());

   /* No type part found! */
   if (ref_idx <= 0)
   {
      return(skip_to_next_statement(end));
   }

   LOG_FMT(LFVD2, "%s:%d TYPE : ", __func__, start->orig_line);
   for (idx = 0; idx < cs.Len() - 1; idx++)
   {
      tmp_pc = cs.Get(idx)->m_pc;
      make_type(tmp_pc);
      tmp_pc->flags |= PCF_VAR_TYPE;
      LOG_FMT(LFVD2, " %s[%s]", tmp_pc->str.c_str(), get_token_name(tmp_pc->type));
   }
   LOG_FMT(LFVD2, "\n");

   /**
    * OK we have two or more items, mark types up to the end.
    */
   mark_variable_definition(cs.Get(cs.Len() - 1)->m_pc, flags);
   if (end->type == CT_COMMA)
   {
      return(chunk_get_next_nnl(end));
   }
   return(skip_to_next_statement(end));
}


/**
 * Skips everything until a comma or semicolon at the same level.
 * Returns the semicolon, comma, or close brace/paren or NULL.
 */
static chunk_t *skip_expression(chunk_t *start)
{
   chunk_t *pc = start;

   while ((pc != NULL) && (pc->level >= start->level))
   {
      if ((pc->level == start->level) &&
          (chunk_is_semicolon(pc) || (pc->type == CT_COMMA)))
      {
         return(pc);
      }
      pc = chunk_get_next_nnl(pc);
   }
   return(pc);
}


/**
 * We are on the first word of a variable definition.
 * Mark all the variable names with PCF_VAR_DECL or PCF_VAR_DEF as appropriate.
 * Also mark any '*' encountered as a CT_PTR_TYPE.
 * Skip over []. Go until a ';' is hit.
 *
 * Example input:
 * int   a = 3, b, c = 2;              ## called with 'a'
 * foo_t f = {1, 2, 3}, g = {5, 6, 7}; ## called with 'f'
 * struct {...} *a, *b;                ## called with 'a' or '*'
 * myclass a(4);
 */
static chunk_t *mark_variable_definition(chunk_t *start, UINT64 flags)
{
   chunk_t *pc   = start;

   if (start == NULL)
   {
      return(NULL);
   }

   LOG_FMT(LVARDEF, "%s: line %d, col %d '%s' type %s\n",
           __func__,
           pc->orig_line, pc->orig_col, pc->str.c_str(),
           get_token_name(pc->type));

   pc = start;
   while ((pc != NULL) && !chunk_is_semicolon(pc) &&
          (pc->level == start->level))
   {
      if ((pc->type == CT_WORD) || (pc->type == CT_FUNC_CTOR_VAR))
      {
         UINT64 flg = pc->flags;
         if ((pc->flags & PCF_IN_ENUM) == 0)
         {
            pc->flags |= flags;
         }

         LOG_FMT(LVARDEF, "%s:%d marked '%s'[%s] in col %d flags: %#" PRIx64 " -> %#" PRIx64 "\n",
                 __func__, pc->orig_line, pc->str.c_str(),
                 get_token_name(pc->type), pc->orig_col, flg, pc->flags);
      }
      else if (chunk_is_star(pc))
      {
         pc->type = CT_PTR_TYPE;
      }
      else if (chunk_is_addr(pc))
      {
         pc->type = CT_BYREF;
      }
      else if ((pc->type == CT_SQUARE_OPEN) || (pc->type == CT_ASSIGN))
      {
         pc = skip_expression(pc);
         continue;
      }
      pc = chunk_get_next_nnl(pc);
   }
   return(pc);
}


/**
 * Checks to see if a series of chunks could be a C++ parameter
 * FOO foo(5, &val);
 *
 * WORD means CT_WORD or CT_TYPE
 *
 * "WORD WORD"          ==> true
 * "QUALIFIER ??"       ==> true
 * "TYPE"               ==> true
 * "WORD"               ==> true
 * "WORD.WORD"          ==> true
 * "WORD::WORD"         ==> true
 * "WORD * WORD"        ==> true
 * "WORD & WORD"        ==> true
 * "NUMBER"             ==> false
 * "STRING"             ==> false
 * "OPEN PAREN"         ==> false
 *
 * @param start the first chunk to look at
 * @param end   the chunk after the last one to look at
 */
static bool can_be_full_param(fp_data& fpd, chunk_t *start, chunk_t *end)
{
   chunk_t *pc;
   chunk_t *last;
   int     word_cnt   = 0;
   int     type_count = 0;
   bool    ret;

   LOG_FMT(LFPARAM, "%s:", __func__);

   for (pc = start; pc != end; pc = chunk_get_next_nnl(pc, CNAV_PREPROC))
   {
      LOG_FMT(LFPARAM, " [%s]", pc->str.c_str());

      if ((pc->type == CT_QUALIFIER) ||
          (pc->type == CT_STRUCT) ||
          (pc->type == CT_ENUM) ||
          (pc->type == CT_UNION) ||
          (pc->type == CT_TYPENAME))
      {
         LOG_FMT(LFPARAM, " <== %s! (yes)\n", get_token_name(pc->type));
         return(true);
      }

      if ((pc->type == CT_WORD) ||
          (pc->type == CT_TYPE))
      {
         word_cnt++;
         if (pc->type == CT_TYPE)
         {
            type_count++;
         }
      }
      else if ((pc->type == CT_MEMBER) ||
               (pc->type == CT_DC_MEMBER))
      {
         if (word_cnt > 0)
         {
            word_cnt--;
         }
      }
      else if ((pc != start) && (chunk_is_star(pc) ||
                                 chunk_is_addr(pc)))
      {
         /* chunk is OK */
      }
      else if (pc->type == CT_ASSIGN)
      {
         /* chunk is OK (default values) */
         break;
      }
      else if (pc->type == CT_ANGLE_OPEN)
      {
         LOG_FMT(LFPARAM, " <== template\n");
         return(true);
      }
      else if (pc->type == CT_ELLIPSIS)
      {
         LOG_FMT(LFPARAM, " <== elipses\n");
         return(true);
      }
      else if ((word_cnt == 0) && (pc->type == CT_PAREN_OPEN))
      {
         /* Check for old-school func proto param '(type)' */
         chunk_t *tmp1 = chunk_skip_to_match(pc, CNAV_PREPROC);
         chunk_t *tmp2 = chunk_get_next_nnl(tmp1, CNAV_PREPROC);

         if (chunk_is_token(tmp2, CT_COMMA) || chunk_is_paren_close(tmp2))
         {

            do {
               pc = chunk_get_next_nnl(pc, CNAV_PREPROC);
               LOG_FMT(LFPARAM, " [%s]", pc->text());
            } while (pc != tmp1);

            /* reset some vars to allow [] after parens */
            word_cnt   = 1;
            type_count = 1;
         }
         else
         {
            LOG_FMT(LFPARAM, " <== [%s] not fcn type!\n", get_token_name(pc->type));
            return false;
         }
      }
      else if (((word_cnt == 1) || (word_cnt == type_count)) &&
               (pc->type == CT_PAREN_OPEN))
      {
         /* Check for func proto param 'void (*name)' or 'void (*name)(params)' */
         chunk_t *tmp1 = chunk_get_next_nnl(pc, CNAV_PREPROC);
         chunk_t *tmp2 = chunk_get_next_nnl(tmp1, CNAV_PREPROC);
         chunk_t *tmp3 = chunk_get_next_nnl(tmp2, CNAV_PREPROC);

         if (!chunk_is_str(tmp3, ")", 1) ||
             !chunk_is_str(tmp1, "*", 1) ||
             (tmp2->type != CT_WORD))
         {
            LOG_FMT(LFPARAM, " <== [%s] not fcn type!\n", get_token_name(pc->type));
            return(false);
         }
         LOG_FMT(LFPARAM, " <skip fcn type>");
         tmp1 = chunk_get_next_nnl(tmp3, CNAV_PREPROC);
         tmp2 = chunk_get_next_nnl(tmp1, CNAV_PREPROC);
         if (chunk_is_str(tmp1, "(", 1))
         {
            tmp3 = chunk_skip_to_match(tmp1, CNAV_PREPROC);
         }
         pc = tmp3;

         /* reset some vars to allow [] after parens */
         word_cnt   = 1;
         type_count = 1;
      }
      else if (pc->type == CT_TSQUARE)
      {
         /* ignore it */
      }
      else if ((word_cnt == 1) && (pc->type == CT_SQUARE_OPEN))
      {
         /* skip over any array stuff */
         pc = chunk_skip_to_match(pc, CNAV_PREPROC);
      }
      else if ((word_cnt == 1) && (fpd.lang_flags & LANG_CPP) &&
               chunk_is_str(pc, "&&", 2))
      {
         /* ignore possible 'move' operator */
      }
      else
      {
         LOG_FMT(LFPARAM, " <== [%s] no way! tc=%d wc=%d\n",
                 get_token_name(pc->type), type_count, word_cnt);
         return(false);
      }
   }

   last = chunk_get_prev_nnl(pc);
   if (chunk_is_star(last) || chunk_is_addr(last))
   {
      LOG_FMT(LFPARAM, " <== [%s] sure!\n", get_token_name(pc->type));
      return(true);
   }

   ret = ((word_cnt >= 2) || ((word_cnt == 1) && (type_count == 1)));

   LOG_FMT(LFPARAM, " <== [%s] %s!\n",
           get_token_name(pc->type), ret ? "Yup" : "Unlikely");
   return(ret);
}


/**
 * We are on a function word. we need to:
 *  - find out if this is a call or prototype or implementation
 *  - mark return type
 *  - mark parameter types
 *  - mark brace pair
 *
 * REVISIT:
 * This whole function is a mess.
 * It needs to be reworked to eliminate duplicate logic and determine the
 * function type more directly.
 *  1. Skip to the close paren and see what is after.
 *     a. semicolon - function call or function proto
 *     b. open brace - function call (ie, list_for_each) or function def
 *     c. open paren - function type or chained function call
 *     d. qualifier - function def or proto, continue to semicolon or open brace
 *  2. Examine the 'parameters' to see if it can be a proto/def
 *  3. Examine what is before the function name to see if it is a proto or call
 * Constructor/destructor detection should have already been done when the
 * 'class' token was encountered (see mark_class_ctor).
 */
static void mark_function(fp_data& fpd, chunk_t *pc)
{
   chunk_t *prev;
   chunk_t *next;
   chunk_t *tmp;
   chunk_t *semi = NULL;
   chunk_t *paren_open;
   chunk_t *paren_close;
   chunk_t *pc_op = NULL;

   prev = chunk_get_prev_nnlnp(pc);
   next = chunk_get_next_nnlnp(pc);

   /* Find out what is before the operator */
   if (pc->parent_type == CT_OPERATOR)
   {
      pc_op = chunk_get_prev_type(pc, CT_OPERATOR, pc->level);
      if ((pc_op != NULL) && (pc_op->flags & PCF_EXPR_START))
      {
         pc->type = CT_FUNC_CALL;
      }
      if (fpd.lang_flags & LANG_CPP)
      {
         tmp = pc;
         while ((tmp = chunk_get_prev_nnl(tmp)) != NULL)
         {
            if ((tmp->type == CT_BRACE_CLOSE) ||
                (tmp->type == CT_SEMICOLON))
            {
               break;
            }
            if (tmp->type == CT_ASSIGN)
            {
               pc->type = CT_FUNC_CALL;
               break;
            }
            if (tmp->type == CT_TEMPLATE)
            {
               pc->type = CT_FUNC_DEF;
               break;
            }
            if (tmp->type == CT_BRACE_OPEN)
            {
               if (tmp->parent_type == CT_FUNC_DEF)
               {
                  pc->type = CT_FUNC_CALL;
               }
               if ((tmp->parent_type == CT_CLASS) ||
                   (tmp->parent_type == CT_STRUCT))
               {
                  pc->type = CT_FUNC_DEF;
               }
               break;
            }
         }
         if ((tmp != NULL) && (pc->type != CT_FUNC_CALL))
         {
            /* Mark the return type */
            while ((tmp = chunk_get_next_nnl(tmp)) != pc)
            {
               make_type(tmp);
            }
         }
      }
   }

   if (chunk_is_star(next) || chunk_is_addr(next))
   {
      next = chunk_get_next_nnlnp(next);
   }

   LOG_FMT(LFCN, "%s: %d] %s[%s] - parent=%s level=%d/%d, next=%s[%s] - level=%d\n",
           __func__,
           pc->orig_line, pc->str.c_str(),
           get_token_name(pc->type), get_token_name(pc->parent_type),
           pc->level, pc->brace_level,
           next->str.c_str(), get_token_name(next->type), next->level);

   if (pc->flags & PCF_IN_CONST_ARGS)
   {
      pc->type = CT_FUNC_CTOR_VAR;
      LOG_FMT(LFCN, "  1) Marked [%s] as FUNC_CTOR_VAR on line %d col %d\n",
              pc->str.c_str(), pc->orig_line, pc->orig_col);
      next = skip_template_next(next);
      flag_parens(next, 0, CT_FPAREN_OPEN, pc->type, true);
      return;
   }

   /* Skip over any template and attribute madness */
   next = skip_template_next(next);
   next = skip_attribute_next(next);

   /* Find the open and close paren */
   paren_open  = chunk_get_next_str(pc, "(", 1, pc->level);
   paren_close = chunk_get_next_str(paren_open, ")", 1, pc->level);

   if ((paren_open == NULL) || (paren_close == NULL))
   {
      LOG_FMT(LFCN, "No parens found for [%s] on line %d col %d\n",
              pc->str.c_str(), pc->orig_line, pc->orig_col);
      return;
   }

   /**
    * This part detects either chained function calls or a function ptr definition.
    * MYTYPE (*func)(void);
    * mWriter( "class Clst_"c )( somestr.getText() )( " : Cluster {"c ).newline;
    *
    * For it to be a function variable def, there must be a '*' followed by a
    * single word.
    *
    * Otherwise, it must be chained function calls.
    */
   tmp = chunk_get_next_nnl(paren_close);
   if (chunk_is_str(tmp, "(", 1))
   {
      chunk_t *tmp1, *tmp2, *tmp3;

      /* skip over any leading class/namespace in: "T(F::*A)();" */
      tmp1 = chunk_get_next_nnl(next);
      while (tmp1)
      {
         tmp2 = chunk_get_next_nnl(tmp1);
         if (!chunk_is_word(tmp1) || !chunk_is_token(tmp2, CT_DC_MEMBER))
         {
            break;
         }
         tmp1 = chunk_get_next_nnl(tmp2);
      }

      tmp2 = chunk_get_next_nnl(tmp1);
      if (chunk_is_str(tmp2, ")", 1))
      {
         tmp3 = tmp2;
         tmp2 = NULL;
      }
      else
      {
         tmp3 = chunk_get_next_nnl(tmp2);
      }

      if (chunk_is_str(tmp3, ")", 1) &&
          (chunk_is_star(tmp1) ||
           ((fpd.lang_flags & LANG_OC) && chunk_is_token(tmp1, CT_CARET)))
           &&
          ((tmp2 == NULL) || (tmp2->type == CT_WORD)))
      {
         if (tmp2)
         {
            LOG_FMT(LFCN, "%s: [%d/%d] function variable [%s], changing [%s] into a type\n",
                    __func__, pc->orig_line, pc->orig_col, tmp2->text(), pc->text());
            tmp2->type = CT_FUNC_VAR;
            flag_parens(paren_open, 0, CT_PAREN_OPEN, CT_FUNC_VAR, false);

            LOG_FMT(LFCN, "%s: paren open @ %d:%d\n",
                    __func__, paren_open->orig_line, paren_open->orig_col);
         }
         else
         {
            LOG_FMT(LFCN, "%s: [%d/%d] function type, changing [%s] into a type\n",
                    __func__, pc->orig_line, pc->orig_col, pc->str.c_str());
            if (tmp2)
            {
               tmp2->type = CT_FUNC_TYPE;
            }
            flag_parens(paren_open, 0, CT_PAREN_OPEN, CT_FUNC_TYPE, false);
         }

         pc->type   = CT_TYPE;
         tmp1->type = CT_PTR_TYPE;
         pc->flags &= ~PCF_VAR_DEF;
         if (tmp2 != NULL)
         {
            tmp2->flags |= PCF_VAR_DEF;
         }
         flag_parens(tmp, 0, CT_FPAREN_OPEN, CT_FUNC_PROTO, false);
         fix_fcn_def_params(fpd, tmp);
         return;
      }

      LOG_FMT(LFCN, "%s: chained function calls? [%d.%d] [%s]\n",
              __func__, pc->orig_line, pc->orig_col, pc->str.c_str());
   }

   /* Assume it is a function call if not already labeled */
   if (pc->type == CT_FUNCTION)
   {
      pc->type = (pc->parent_type == CT_OPERATOR) ? CT_FUNC_DEF : CT_FUNC_CALL;
   }

   /* Check for C++ function def */
   if ((pc->type == CT_FUNC_CLASS) ||
       ((prev != NULL) && ((prev->type == CT_DC_MEMBER) ||
                           (prev->type == CT_INV))))
   {
      chunk_t *destr = NULL;
      if (prev->type == CT_INV)
      {
         /* TODO: do we care that this is the destructor? */
         prev->type = CT_DESTRUCTOR;
         pc->type   = CT_FUNC_CLASS;

         pc->parent_type = CT_DESTRUCTOR;

         destr = prev;
         prev  = chunk_get_prev_nnlnp(prev);
      }

      if ((prev != NULL) && (prev->type == CT_DC_MEMBER))
      {
         prev = chunk_get_prev_nnlnp(prev);
         // LOG_FMT(LNOTE, "%s: prev1 = %s (%s)\n", __func__,
         //         get_token_name(prev->type), prev->str.c_str());
         prev = skip_template_prev(prev);
         prev = skip_attribute_prev(prev);
         // LOG_FMT(LNOTE, "%s: prev2 = %s [%d](%s) pc = %s [%d](%s)\n", __func__,
         //         get_token_name(prev->type), prev->len, prev->str.c_str(),
         //         get_token_name(pc->type), pc->len, pc->str.c_str());
         if ((prev != NULL) && ((prev->type == CT_WORD) || (prev->type == CT_TYPE)))
         {
            if (pc->str == prev->str)
            {
               pc->type = CT_FUNC_CLASS;
               LOG_FMT(LFCN, "FOUND %sSTRUCTOR for %s[%s]\n",
                       (destr != NULL) ? "DE" : "CON",
                       prev->str.c_str(), get_token_name(prev->type));

               mark_cpp_constructor(fpd, pc);
               return;
            }
            else
            {
               /* Point to the item previous to the class name */
               prev = chunk_get_prev_nnlnp(prev);
            }
         }
      }
   }

   /* Determine if this is a function call or a function def/proto
    * We check for level==1 to allow the case that a function prototype is
    * wrapped in a macro: "MACRO(void foo(void));"
    */
   if ((pc->type == CT_FUNC_CALL) &&
       ((pc->level == pc->brace_level) || (pc->level == 1)) &&
       ((pc->flags & PCF_IN_ARRAY_ASSIGN) == 0))
   {
      bool isa_def  = false;
      bool hit_star = false;
      LOG_FMT(LFCN, "  Checking func call: prev=%s", (prev == NULL) ? "<null>" : get_token_name(prev->type));

      // if (!chunk_ends_type(prev))
      // {
      //    goto bad_ret_type;
      // }

      /**
       * REVISIT:
       * a function def can only occur at brace level, but not inside an
       * assignment, structure, enum, or union.
       * The close paren must be followed by an open brace, with an optional
       * qualifier (const) in between.
       * There can be all sorts of template crap and/or '[]' in the type.
       * This hack mostly checks that.
       *
       * Examples:
       * foo->bar(maid);                   -- fcn call
       * FOO * bar();                      -- fcn proto or class variable
       * FOO foo();                        -- fcn proto or class variable
       * FOO foo(1);                       -- class variable
       * a = FOO * bar();                  -- fcn call
       * a.y = foo() * bar();              -- fcn call
       * static const char * const fizz(); -- fcn def
       */
      while (prev != NULL)
      {
         if (prev->flags & PCF_IN_PREPROC)
         {
            prev = chunk_get_prev_nnlnp(prev);
            continue;
         }

         /* Some code slips an attribute between the type and function */
         if ((prev->type == CT_FPAREN_CLOSE) &&
             (prev->parent_type == CT_ATTRIBUTE))
         {
            prev = skip_attribute_prev(prev);
            continue;
         }

         /* skip const(TYPE) */
         if ((prev->type == CT_PAREN_CLOSE) &&
             (prev->parent_type == CT_D_CAST))
         {
            LOG_FMT(LFCN, " --> For sure a prototype or definition\n");
            isa_def = true;
            break;
         }

         /** Skip the word/type before the '.' or '::' */
         if ((prev->type == CT_DC_MEMBER) ||
             (prev->type == CT_MEMBER))
         {
            prev = chunk_get_prev_nnlnp(prev);
            if ((prev == NULL) ||
                ((prev->type != CT_WORD) &&
                 (prev->type != CT_TYPE) &&
                 (prev->type != CT_THIS)))
            {
               LOG_FMT(LFCN, " --? Skipped MEMBER and landed on %s\n",
                       (prev == NULL) ? "<null>" : get_token_name(prev->type));
               pc->type = CT_FUNC_CALL;
               isa_def  = false;
               break;
            }
            LOG_FMT(LFCN, " <skip %s>", prev->str.c_str());
            prev = chunk_get_prev_nnlnp(prev);
            continue;
         }

         /* If we are on a TYPE or WORD, then we must be on a proto or def */
         if ((prev->type == CT_TYPE) ||
             (prev->type == CT_WORD))
         {
            if (!hit_star)
            {
               LOG_FMT(LFCN, " --> For sure a prototype or definition\n");
               isa_def = true;
               break;
            }
            LOG_FMT(LFCN, " --> maybe a proto/def\n");
            isa_def = true;
         }

         if (chunk_is_addr(prev) ||
             chunk_is_star(prev))
         {
            hit_star = true;
         }

         if ((prev->type != CT_OPERATOR) &&
             (prev->type != CT_TSQUARE) &&
             (prev->type != CT_ANGLE_CLOSE) &&
             (prev->type != CT_QUALIFIER) &&
             (prev->type != CT_TYPE) &&
             (prev->type != CT_WORD) &&
             !chunk_is_addr(prev) &&
             !chunk_is_star(prev))
         {
            LOG_FMT(LFCN, " --> Stopping on %s [%s]\n",
                    prev->str.c_str(), get_token_name(prev->type));
            /* certain tokens are unlikely to preceed a proto or def */
            if ((prev->type == CT_ARITH) ||
                (prev->type == CT_ASSIGN) ||
                (prev->type == CT_COMMA) ||
                (prev->type == CT_STRING) ||
                (prev->type == CT_STRING_MULTI) ||
                (prev->type == CT_NUMBER) ||
                (prev->type == CT_NUMBER_FP))
            {
               isa_def = false;
            }
            break;
         }

         /* Skip over template and attribute stuff */
         if (prev->type == CT_ANGLE_CLOSE)
         {
            prev = skip_template_prev(prev);
         }
         else
         {
            prev = chunk_get_prev_nnlnp(prev);
         }
      }

      //LOG_FMT(LFCN, " -- stopped on %s [%s]\n",
      //        prev->str.c_str(), get_token_name(prev->type));

      if (isa_def && (prev != NULL) &&
          ((chunk_is_paren_close(prev) && (prev->parent_type != CT_D_CAST)) ||
           (prev->type == CT_ASSIGN) ||
           (prev->type == CT_RETURN)))
      {
         LOG_FMT(LFCN, " -- overriding DEF due to %s [%s]\n",
                 prev->str.c_str(), get_token_name(prev->type));
         isa_def = false;
      }
      if (isa_def)
      {
         pc->type = CT_FUNC_DEF;
         LOG_FMT(LFCN, "%s: '%s' is FCN_DEF:", __func__, pc->str.c_str());
         if (prev == NULL)
         {
            prev = chunk_get_head(fpd);
         }
         for (tmp = prev; tmp != pc; tmp = chunk_get_next_nnl(tmp))
         {
            LOG_FMT(LFCN, " %s[%s]",
                    tmp->str.c_str(), get_token_name(tmp->type));
            make_type(tmp);
         }
         LOG_FMT(LFCN, "\n");
      }
   }

   if (pc->type != CT_FUNC_DEF)
   {
      LOG_FMT(LFCN, "  Detected %s '%s' on line %d col %d\n",
              get_token_name(pc->type),
              pc->str.c_str(), pc->orig_line, pc->orig_col);

      tmp = flag_parens(next, PCF_IN_FCN_CALL, CT_FPAREN_OPEN, CT_FUNC_CALL, false);
      if ((tmp != NULL) && (tmp->type == CT_BRACE_OPEN))
      {
         set_paren_parent(tmp, pc->type);
      }
      return;
   }

   /* We have a function definition or prototype
    * Look for a semicolon or a brace open after the close paren to figure
    * out whether this is a prototype or definition
    */

   /* See if this is a prototype or implementation */

   /* FIXME: this doesn't take the old K&R parameter definitions into account */

   /* Scan tokens until we hit a brace open (def) or semicolon (proto) */
   tmp = paren_close;
   while ((tmp = chunk_get_next_nnl(tmp)) != NULL)
   {
      /* Only care about brace or semi on the same level */
      if (tmp->level < pc->level)
      {
         /* No semicolon - guess that it is a prototype */
         pc->type = CT_FUNC_PROTO;
         break;
      }
      else if (tmp->level == pc->level)
      {
         if (tmp->type == CT_BRACE_OPEN)
         {
            /* its a function def for sure */
            break;
         }
         else if (chunk_is_semicolon(tmp))
         {
            /* Set the parent for the semi for later */
            semi     = tmp;
            pc->type = CT_FUNC_PROTO;
            break;
         }
         else if (pc->type == CT_COMMA)
         {
            pc->type = CT_FUNC_CTOR_VAR;
            LOG_FMT(LFCN, "  2) Marked [%s] as FUNC_CTOR_VAR on line %d col %d\n",
                    pc->str.c_str(), pc->orig_line, pc->orig_col);
            break;
         }
      }
   }

   /**
    * C++ syntax is wacky. We need to check to see if a prototype is really a
    * variable definition with parameters passed into the constructor.
    * Unfortunately, the only mostly reliable way to do so is to guess that
    * it is a constructor variable if inside a function body and scan the
    * 'parameter list' for items that are not allowed in a prototype.
    * We search backwards and checking the parent of the containing open braces.
    * If the parent is a class or namespace, then it probably is a prototype.
    */
   if ((fpd.lang_flags & LANG_CPP) &&
       (pc->type == CT_FUNC_PROTO) &&
       (pc->parent_type != CT_OPERATOR))
   {
      LOG_FMT(LFPARAM, "%s :: checking '%s' for constructor variable %s %s\n",
              __func__, pc->str.c_str(),
              get_token_name(paren_open->type),
              get_token_name(paren_close->type));

      /* Scan the parameters looking for:
       *  - constant strings
       *  - numbers
       *  - non-type fields
       *  - function calls
       */
      chunk_t *ref = chunk_get_next_nnl(paren_open);
      chunk_t *tmp2;
      bool    is_param = true;
      tmp = ref;
      while (tmp != paren_close)
      {
         tmp2 = chunk_get_next_nnl(tmp);
         if ((tmp->type == CT_COMMA) && (tmp->level == (paren_open->level + 1)))
         {
            if (!can_be_full_param(fpd, ref, tmp))
            {
               is_param = false;
               break;
            }
            ref = tmp2;
         }
         tmp = tmp2;
      }
      if (is_param && (ref != tmp))
      {
         if (!can_be_full_param(fpd, ref, tmp))
         {
            is_param = false;
         }
      }
      if (!is_param)
      {
         pc->type = CT_FUNC_CTOR_VAR;
         LOG_FMT(LFCN, "  3) Marked [%s] as FUNC_CTOR_VAR on line %d col %d\n",
                 pc->str.c_str(), pc->orig_line, pc->orig_col);
      }
      else if (pc->brace_level > 0)
      {
         chunk_t *br_open = chunk_get_prev_type(pc, CT_BRACE_OPEN, pc->brace_level - 1);

         if ((br_open != NULL) &&
             (br_open->parent_type != CT_EXTERN) &&
             (br_open->parent_type != CT_NAMESPACE))
         {
            /* Do a check to see if the level is right */
            prev = chunk_get_prev_nnl(pc);
            if (!chunk_is_str(prev, "*", 1) && !chunk_is_str(prev, "&", 1))
            {
               chunk_t *p_op = chunk_get_prev_type(pc, CT_BRACE_OPEN, pc->brace_level - 1);
               if ((p_op != NULL) &&
                   (p_op->parent_type != CT_CLASS) &&
                   (p_op->parent_type != CT_STRUCT) &&
                   (p_op->parent_type != CT_NAMESPACE))
               {
                  pc->type = CT_FUNC_CTOR_VAR;
                  LOG_FMT(LFCN, "  4) Marked [%s] as FUNC_CTOR_VAR on line %d col %d\n",
                          pc->str.c_str(), pc->orig_line, pc->orig_col);
               }
            }
         }
      }
   }

   if (semi != NULL)
   {
      semi->parent_type = pc->type;
   }

   flag_parens(paren_open, PCF_IN_FCN_DEF, CT_FPAREN_OPEN, pc->type, false);

   if (pc->type == CT_FUNC_CTOR_VAR)
   {
      pc->flags |= PCF_VAR_DEF;
      return;
   }

   if (next->type == CT_TSQUARE)
   {
      next = chunk_get_next_nnl(next);
   }

   /* Mark parameters and return type */
   fix_fcn_def_params(fpd, next);
   mark_function_return_type(pc, chunk_get_prev_nnl(pc), pc->type);

   /* Find the brace pair and set the parent */
   if (pc->type == CT_FUNC_DEF)
   {
      tmp = chunk_get_next_nnl(paren_close, CNAV_PREPROC);
      if ((tmp != NULL) && (tmp->type == CT_BRACE_OPEN))
      {
         tmp->parent_type = CT_FUNC_DEF;
         tmp = chunk_skip_to_match(tmp);
         if (tmp != NULL)
         {
            tmp->parent_type = CT_FUNC_DEF;
         }
      }
   }
}


static void mark_cpp_constructor(fp_data& fpd, chunk_t *pc)
{
   chunk_t *paren_open;
   chunk_t *tmp;
   chunk_t *after;
   chunk_t *var;

   tmp = chunk_get_prev_nnl(pc);
   if (tmp->type == CT_INV)
   {
      tmp->type       = CT_DESTRUCTOR;
      pc->parent_type = CT_DESTRUCTOR;
   }

   LOG_FMT(LFTOR, "%d:%d FOUND %sSTRUCTOR for %s[%s]",
           pc->orig_line, pc->orig_col,
           tmp->type == CT_DESTRUCTOR ? "DE" : "CON",
           pc->str.c_str(), get_token_name(pc->type));

   paren_open = skip_template_next(chunk_get_next_nnl(pc));
   if (!chunk_is_str(paren_open, "(", 1))
   {
      LOG_FMT(LWARN, "%s:%d Expected '(', got: [%s]\n",
              fpd.filename, paren_open->orig_line,
              paren_open->str.c_str());
      return;
   }

   /* Mark parameters */
   fix_fcn_def_params(fpd, paren_open);
   after = flag_parens(paren_open, PCF_IN_FCN_CALL, CT_FPAREN_OPEN, CT_FUNC_CLASS, false);

   LOG_FMT(LFTOR, "[%s]\n", after->str.c_str());

   /* Scan until the brace open, mark everything */
   tmp = paren_open;
   bool hit_colon = false;
   while ((tmp != NULL) && (tmp->type != CT_BRACE_OPEN) &&
          !chunk_is_semicolon(tmp))
   {
      tmp->flags |= PCF_IN_CONST_ARGS;
      tmp         = chunk_get_next_nnl(tmp);
      if (chunk_is_str(tmp, ":", 1) && (tmp->level == paren_open->level))
      {
         tmp->type = CT_CONSTR_COLON;
         hit_colon = true;
      }
      if (hit_colon &&
          (chunk_is_paren_open(tmp) ||
           chunk_is_opening_brace(tmp)) &&
          (tmp->level == paren_open->level))
      {
         var = skip_template_prev(chunk_get_prev_nnl(tmp));
         if ((var->type == CT_TYPE) || (var->type == CT_WORD))
         {
            var->type = CT_FUNC_CTOR_VAR;
            flag_parens(tmp, PCF_IN_FCN_CALL, CT_FPAREN_OPEN, CT_FUNC_CTOR_VAR, false);
         }
      }
   }
   if ((tmp != NULL) && (tmp->type == CT_BRACE_OPEN))
   {
      set_paren_parent(tmp, CT_FUNC_CLASS);
      pc->flags |= PCF_DEF;
   }
   else
   {
      pc->flags |= PCF_PROTO;
   }
}


/**
 * We're on a 'class' or 'struct'.
 * Scan for CT_FUNCTION with a string that matches pclass->str
 */
static void mark_class_ctor(fp_data& fpd, chunk_t *start)
{
   chunk_t    *next;
   chunk_t    *pclass;
   ChunkStack cs;

   pclass = chunk_get_next_nnl(start, CNAV_PREPROC);
   if ((pclass == NULL) ||
       ((pclass->type != CT_TYPE) &&
        (pclass->type != CT_WORD)))
   {
      return;
   }

   next = chunk_get_next_nnl(pclass, CNAV_PREPROC);
   while ((next != NULL) &&
          ((next->type == CT_TYPE) ||
           (next->type == CT_WORD) ||
           (next->type == CT_DC_MEMBER)))
   {
      pclass = next;
      next   = chunk_get_next_nnl(next, CNAV_PREPROC);
   }

   chunk_t *pc   = chunk_get_next_nnl(pclass, CNAV_PREPROC);
   int     level = pclass->brace_level + 1;

   if (pc == NULL)
   {
      LOG_FMT(LFTOR, "%s: Called on %s on line %d. Bailed on NULL\n",
              __func__, pclass->str.c_str(), pclass->orig_line);
      return;
   }

   /* Add the class name */
   cs.Push_Back(pclass);

   LOG_FMT(LFTOR, "%s: Called on %s on line %d (next='%s')\n",
           __func__, pclass->str.c_str(), pclass->orig_line, pc->str.c_str());

   /* detect D template class: "class foo(x) { ... }" */
   if ((fpd.lang_flags & LANG_D) && (next->type == CT_PAREN_OPEN))
   {
      next->parent_type = CT_TEMPLATE;

      next = get_d_template_types(cs, next);
      if (next && (next->type == CT_PAREN_CLOSE))
      {
         next->parent_type = CT_TEMPLATE;
      }
   }

   /* Find the open brace, abort on semicolon */
   int flags = 0;
   while ((pc != NULL) && (pc->type != CT_BRACE_OPEN))
   {
      LOG_FMT(LFTOR, " [%s]", pc->str.c_str());

      if (chunk_is_str(pc, ":", 1))
      {
         pc->type = CT_CLASS_COLON;
         flags |= PCF_IN_CLASS_BASE;
         LOG_FMT(LFTOR, "%s: class colon on line %d\n",
                 __func__, pc->orig_line);
      }

      if (chunk_is_semicolon(pc))
      {
         LOG_FMT(LFTOR, "%s: bailed on semicolon on line %d\n",
                 __func__, pc->orig_line);
         pclass->flags |= PCF_PROTO;
         return;
      }
      pc->flags |= flags;
      pc = chunk_get_next_nnl(pc, CNAV_PREPROC);
   }

   if (pc == NULL)
   {
      LOG_FMT(LFTOR, "%s: bailed on NULL\n", __func__);
      return;
   }

   pclass->flags |= PCF_DEF;

   set_paren_parent(pc, start->type);

   pc = chunk_get_next_nnl(pc, CNAV_PREPROC);
   while (pc != NULL)
   {
      pc->flags |= PCF_IN_CLASS;

      if ((pc->brace_level > level) || ((pc->flags & PCF_IN_PREPROC) != 0))
      {
         pc = chunk_get_next_nnl(pc);
         continue;
      }

      if ((pc->type == CT_BRACE_CLOSE) && (pc->brace_level < level))
      {
         LOG_FMT(LFTOR, "%s: %d] Hit brace close\n", __func__, pc->orig_line);
         pc = chunk_get_next_nnl(pc, CNAV_PREPROC);
         if (pc && (pc->type == CT_SEMICOLON))
         {
            pc->parent_type = start->type;
         }
         return;
      }

      next = chunk_get_next_nnl(pc, CNAV_PREPROC);
      if (chunkstack_match(cs, pc))
      {
         if ((next != NULL) && (next->len() == 1) && (next->str[0] == '('))
         {
            pc->type = CT_FUNC_CLASS;
            LOG_FMT(LFTOR, "%d] Marked CTor/DTor %s\n", pc->orig_line, pc->str.c_str());
            mark_cpp_constructor(fpd, pc);
         }
         else
         {
            make_type(pc);
         }
      }
      pc = next;
   }
}


/**
 * We're on a 'namespace' skip the word and then set the parent of the braces.
 */
static void mark_namespace(chunk_t *pns)
{
   chunk_t *pc;
   bool    is_using = false;

   pc = chunk_get_prev_nnl(pns);
   if (chunk_is_token(pc, CT_USING))
   {
      is_using = true;
      pns->parent_type = CT_USING;
   }

   pc = chunk_get_next_nnl(pns);
   if (pc->type == CT_WORD)
   {
      if (is_using)
         pc->flags |= PCF_REF;
      else
         pc->flags |= PCF_DEF;
   }
   while (pc != NULL)
   {
      pc->parent_type = CT_NAMESPACE;
      if (pc->type != CT_BRACE_OPEN)
      {
         if (pc->type == CT_SEMICOLON)
         {
            if (is_using)
            {
               pc->parent_type = CT_USING;
            }
            return;
         }
         pc = chunk_get_next_nnl(pc);
         continue;
      }

      flag_parens(pc, PCF_IN_NAMESPACE, CT_NONE, CT_NAMESPACE, false);
      return;
   }
}


/**
 * Skips the D 'align()' statement and the colon, if present.
 *    align(2) int foo;  -- returns 'int'
 *    align(4):          -- returns 'int'
 *    int bar;
 */
static chunk_t *skip_align(chunk_t *start)
{
   chunk_t *pc = start;

   if (pc->type == CT_ALIGN)
   {
      pc = chunk_get_next_nnl(pc);
      if (pc->type == CT_PAREN_OPEN)
      {
         pc = chunk_get_next_type(pc, CT_PAREN_CLOSE, pc->level);
         pc = chunk_get_next_nnl(pc);
         if (pc->type == CT_COLON)
         {
            pc = chunk_get_next_nnl(pc);
         }
      }
   }
   return(pc);
}


/**
 * Examines the stuff between braces { }.
 * There should only be variable definitions and methods.
 * Skip the methods, as they will get handled elsewhere.
 */
static void mark_struct_union_body(chunk_t *start)
{
   chunk_t *pc = start;

   while ((pc != NULL) &&
          (pc->level >= start->level) &&
          !((pc->level == start->level) && (pc->type == CT_BRACE_CLOSE)))
   {
      // LOG_FMT(LNOTE, "%s: %d:%d %s:%s\n", __func__, pc->orig_line, pc->orig_col,
      //         pc->str.c_str(), get_token_name(pc->parent_type));
      if ((pc->type == CT_BRACE_OPEN) ||
          (pc->type == CT_BRACE_CLOSE) ||
          (pc->type == CT_SEMICOLON))
      {
         pc = chunk_get_next_nnl(pc);
      }
      if (pc->type == CT_ALIGN)
      {
         pc = skip_align(pc); // "align(x)" or "align(x):"
      }
      else
      {
         pc = fix_var_def(pc);
      }
   }
}


/**
 * Marks statement starts in a macro body.
 * REVISIT: this may already be done
 */
static void mark_define_expressions(fp_data& fpd)
{
   chunk_t *pc;
   chunk_t *prev;
   bool    in_define = false;
   bool    first     = true;

   pc   = chunk_get_head(fpd);
   prev = pc;

   while (pc != NULL)
   {
      if (!in_define)
      {
         if ((pc->type == CT_PP_DEFINE) ||
             (pc->type == CT_PP_IF) ||
             (pc->type == CT_PP_ELSE))
         {
            in_define = true;
            first     = true;
         }
      }
      else
      {
         if (((pc->flags & PCF_IN_PREPROC) == 0) || (pc->type == CT_PREPROC))
         {
            in_define = false;
         }
         else
         {
            if ((pc->type != CT_MACRO) &&
                (first ||
                 (prev->type == CT_PAREN_OPEN) ||
                 (prev->type == CT_ARITH) ||
                 (prev->type == CT_CARET) ||
                 (prev->type == CT_ASSIGN) ||
                 (prev->type == CT_COMPARE) ||
                 (prev->type == CT_RETURN) ||
                 (prev->type == CT_GOTO) ||
                 (prev->type == CT_CONTINUE) ||
                 (prev->type == CT_PAREN_OPEN) ||
                 (prev->type == CT_FPAREN_OPEN) ||
                 (prev->type == CT_SPAREN_OPEN) ||
                 (prev->type == CT_BRACE_OPEN) ||
                 chunk_is_semicolon(prev) ||
                 (prev->type == CT_COMMA) ||
                 (prev->type == CT_COLON) ||
                 (prev->type == CT_QUESTION)))
            {
               pc->flags |= PCF_EXPR_START;
               first      = false;
            }
         }
      }

      prev = pc;
      pc   = chunk_get_next(pc);
   }
}


/**
 * We are on the C++ 'template' keyword.
 * What follows should be the following:
 *
 * template <class identifier> function_declaration;
 * template <typename identifier> function_declaration;
 * template <class identifier> class class_declaration;
 * template <typename identifier> class class_declaration;
 *
 * Change the 'class' inside the <> to CT_TYPE.
 * Set the parent to the class after the <> to CT_TEMPLATE.
 * Set the parent of the semicolon to CT_TEMPLATE.
 */
static void handle_cpp_template(chunk_t *pc)
{
   chunk_t *tmp;
   int     level;

   tmp = chunk_get_next_nnl(pc);
   if (tmp->type != CT_ANGLE_OPEN)
   {
      return;
   }
   tmp->parent_type = CT_TEMPLATE;

   level = tmp->level;

   while ((tmp = chunk_get_next(tmp)) != NULL)
   {
      if ((tmp->type == CT_CLASS) ||
          (tmp->type == CT_STRUCT))
      {
         tmp->type = CT_TYPE;
      }
      else if ((tmp->type == CT_ANGLE_CLOSE) && (tmp->level == level))
      {
         tmp->parent_type = CT_TEMPLATE;
         break;
      }
   }
   if (tmp != NULL)
   {
      tmp = chunk_get_next_nnl(tmp);
      if ((tmp != NULL) &&
          ((tmp->type == CT_CLASS) || (tmp->type == CT_STRUCT)))
      {
         tmp->parent_type = CT_TEMPLATE;

         /* REVISTI: This may be a bit risky - might need to track the { }; */
         tmp = chunk_get_next_type(tmp, CT_SEMICOLON, tmp->level);
         if (tmp != NULL)
         {
            tmp->parent_type = CT_TEMPLATE;
         }
      }
   }
}


/**
 * Verify and then mark C++ lambda expressions.
 * The expected format is '[...](...){...}' or '[...](...) -> type {...}'
 * sq_o is '[' CT_SQUARE_OPEN or '[]' CT_TSQUARE
 * Split the '[]' so we can control the space
 */
static void handle_cpp_lambda(fp_data& fpd, chunk_t *sq_o)
{
   chunk_t *sq_c;
   chunk_t *pa_o;
   chunk_t *pa_c;
   chunk_t *br_o;
   chunk_t *br_c;
   chunk_t *ret = NULL;

   sq_c = sq_o; /* assuming '[]' */
   if (sq_o->type == CT_SQUARE_OPEN)
   {
      /* make sure there is a ']' */
      sq_c = chunk_skip_to_match(sq_o);
      if (!sq_c)
      {
         return;
      }
   }

   /* Make sure a '(' is next */
   pa_o = chunk_get_next_nnl(sq_c);
   if (!pa_o || (pa_o->type != CT_PAREN_OPEN))
   {
      return;
   }
   /* and now find the ')' */
   pa_c = chunk_skip_to_match(pa_o);
   if (!pa_c)
   {
      return;
   }

   /* Check if keyword 'mutable' is before '->' */
   br_o = chunk_get_next_nnl(pa_c);
   if (chunk_is_str(br_o, "mutable", 7))
   {
      br_o = chunk_get_next_nnl(br_o);
   }

   /* Make sure a '{' or '->' is next */
   if (chunk_is_str(br_o, "->", 2))
   {
      ret = br_o;
      /* REVISIT: really should check the stuff we are skipping */
      br_o = chunk_get_next_type(br_o, CT_BRACE_OPEN, br_o->level);
   }
   if (!br_o || (br_o->type != CT_BRACE_OPEN))
   {
      return;
   }
   /* and now find the '}' */
   br_c = chunk_skip_to_match(br_o);
   if (!br_c)
   {
      return;
   }

   /* This looks like a lambda expression */
   if (sq_o->type == CT_TSQUARE)
   {
      /* split into two chunks */
      chunk_t nc;

      nc         = *sq_o;
      sq_o->type = CT_SQUARE_OPEN;
      sq_o->str.resize(1);
      sq_o->orig_col_end = sq_o->orig_col + 1;

      nc.type = CT_SQUARE_CLOSE;
      nc.str = "]";
      nc.orig_col++;
      sq_c = chunk_add_after(fpd, &nc, sq_o);
   }
   sq_o->parent_type = CT_CPP_LAMBDA;
   sq_c->parent_type = CT_CPP_LAMBDA;
   pa_o->type        = CT_FPAREN_OPEN;
   pa_o->parent_type = CT_CPP_LAMBDA;
   pa_c->type        = CT_FPAREN_CLOSE;
   pa_c->parent_type = CT_CPP_LAMBDA;
   br_o->parent_type = CT_CPP_LAMBDA;
   br_c->parent_type = CT_CPP_LAMBDA;

   if (ret)
   {
      ret->type = CT_CPP_LAMBDA_RET;
      ret       = chunk_get_next_nnl(ret);
      while (ret != br_o)
      {
         make_type(ret);
         ret = chunk_get_next_nnl(ret);
      }
   }

   fix_fcn_def_params(fpd, pa_o);
}


/**
 * Parse off the types in the D template args, adds to cs
 * returns the close_paren
 */
static chunk_t *get_d_template_types(ChunkStack& cs, chunk_t *open_paren)
{
   chunk_t *tmp       = open_paren;
   bool    maybe_type = true;

   while (((tmp = chunk_get_next_nnl(tmp)) != NULL) &&
          (tmp->level > open_paren->level))
   {
      if ((tmp->type == CT_TYPE) || (tmp->type == CT_WORD))
      {
         if (maybe_type)
         {
            make_type(tmp);
            cs.Push_Back(tmp);
         }
         maybe_type = false;
      }
      else if (tmp->type == CT_COMMA)
      {
         maybe_type = true;
      }
   }
   return tmp;
}


static bool chunkstack_match(ChunkStack& cs, chunk_t *pc)
{
   chunk_t *tmp;
   int     idx;

   for (idx = 0; idx < cs.Len(); idx++)
   {
      tmp = cs.GetChunk(idx);
      if (pc->str == tmp->str)
      {
         return true;
      }
   }
   return false;
}


/**
 * We are on the D 'template' keyword.
 * What follows should be the following:
 *
 * template NAME ( TYPELIST ) { BODY }
 *
 * Set the parent of NAME to template, change NAME to CT_TYPE.
 * Set the parent of the parens and braces to CT_TEMPLATE.
 * Scan the body for each type in TYPELIST and change the type to CT_TYPE.
 */
static void handle_d_template(chunk_t *pc)
{
   chunk_t *name;
   chunk_t *po;
   chunk_t *tmp;

   name = chunk_get_next_nnl(pc);
   po   = chunk_get_next_nnl(name);
   if (!name || ((name->type != CT_WORD) && (name->type != CT_WORD)))
   {
      /* TODO: log an error, expected NAME */
      return;
   }
   if (!po || (po->type != CT_PAREN_OPEN))
   {
      /* TODO: log an error, expected '(' */
      return;
   }

   name->type        = CT_TYPE;
   name->parent_type = CT_TEMPLATE;
   po->parent_type   = CT_TEMPLATE;

   ChunkStack cs;

   tmp = get_d_template_types(cs, po);

   if (!tmp || (tmp->type != CT_PAREN_CLOSE))
   {
      /* TODO: log an error, expected ')' */
      return;
   }
   tmp->parent_type = CT_TEMPLATE;

   tmp = chunk_get_next_nnl(tmp);
   if (tmp->type != CT_BRACE_OPEN)
   {
      /* TODO: log an error, expected '{' */
      return;
   }
   tmp->parent_type = CT_TEMPLATE;
   po = tmp;

   tmp = po;
   while (((tmp = chunk_get_next_nnl(tmp)) != NULL) &&
          (tmp->level > po->level))
   {
      if ((tmp->type == CT_WORD) && chunkstack_match(cs, tmp))
      {
         tmp->type = CT_TYPE;
      }
   }
   if (tmp->type != CT_BRACE_CLOSE)
   {
      /* TODO: log an error, expected '}' */
   }
   tmp->parent_type = CT_TEMPLATE;
}


/**
 * We are on a word followed by a angle open which is part of a template.
 * If the angle close is followed by a open paren, then we are on a template
 * function def or a template function call:
 *   Vector2<float>(...) [: ...[, ...]] { ... }
 * Or we could be on a variable def if it's followed by a word:
 *   Renderer<rgb32> rend;
 */
static void mark_template_func(fp_data& fpd, chunk_t *pc, chunk_t *pc_next)
{
   chunk_t *angle_close;
   chunk_t *after;

   /* We know angle_close must be there... */
   angle_close = chunk_get_next_type(pc_next, CT_ANGLE_CLOSE, pc->level);

   after = chunk_get_next_nnl(angle_close);
   if (after != NULL)
   {
      if (chunk_is_str(after, "(", 1))
      {
         if (angle_close->flags & PCF_IN_FCN_CALL)
         {
            LOG_FMT(LTEMPFUNC, "%s: marking '%s' in line %d as a FUNC_CALL\n",
                    __func__, pc->str.c_str(), pc->orig_line);
            pc->type = CT_FUNC_CALL;
            flag_parens(after, PCF_IN_FCN_CALL, CT_FPAREN_OPEN, CT_FUNC_CALL, false);
         }
         else
         {
            /* Might be a function def. Must check what is before the template:
             * Func call:
             *   BTree.Insert(std::pair<int, double>(*it, double(*it) + 1.0));
             *   a = Test<int>(j);
             *   std::pair<int, double>(*it, double(*it) + 1.0));
             */

            LOG_FMT(LTEMPFUNC, "%s: marking '%s' in line %d as a FUNC_CALL 2\n",
                    __func__, pc->str.c_str(), pc->orig_line);
            // its a function!!!
            pc->type = CT_FUNC_CALL;
            mark_function(fpd, pc);
         }
      }
      else if (after->type == CT_WORD)
      {
         // its a type!
         pc->type      = CT_TYPE;
         pc->flags    |= PCF_VAR_TYPE;
         after->flags |= PCF_VAR_DEF;
      }
   }
}


/**
 * Just mark every CT_WORD until a semicolon as CT_SQL_WORD.
 * Adjust the levels if pc is CT_SQL_BEGIN
 */
static void mark_exec_sql(chunk_t *pc)
{
   chunk_t *tmp;

   /* Change CT_WORD to CT_SQL_WORD */
   for (tmp = chunk_get_next(pc); tmp != NULL; tmp = chunk_get_next(tmp))
   {
      tmp->parent_type = pc->type;
      if (tmp->type == CT_WORD)
      {
         tmp->type = CT_SQL_WORD;
      }
      if (tmp->type == CT_SEMICOLON)
      {
         break;
      }
   }

   if ((pc->type != CT_SQL_BEGIN) ||
       (tmp == NULL) || (tmp->type != CT_SEMICOLON))
   {
      return;
   }

   for (tmp = chunk_get_next(tmp);
        (tmp != NULL) && (tmp->type != CT_SQL_END);
        tmp = chunk_get_next(tmp))
   {
      tmp->level++;
   }
}


/**
 * Skips over the rest of the template if ang_open is indeed a CT_ANGLE_OPEN.
 * Points to the chunk after the CT_ANGLE_CLOSE.
 * If the chunk isn't an CT_ANGLE_OPEN, then it is returned.
 */
chunk_t *skip_template_next(chunk_t *ang_open)
{
   if ((ang_open != NULL) && (ang_open->type == CT_ANGLE_OPEN))
   {
      chunk_t *pc;
      pc = chunk_get_next_type(ang_open, CT_ANGLE_CLOSE, ang_open->level);
      return(chunk_get_next_nnl(pc));
   }
   return(ang_open);
}


/**
 * Skips over the rest of the template if ang_close is indeed a CT_ANGLE_CLOSE.
 * Points to the chunk before the CT_ANGLE_OPEN
 * If the chunk isn't an CT_ANGLE_CLOSE, then it is returned.
 */
chunk_t *skip_template_prev(chunk_t *ang_close)
{
   if ((ang_close != NULL) && (ang_close->type == CT_ANGLE_CLOSE))
   {
      chunk_t *pc;
      pc = chunk_get_prev_type(ang_close, CT_ANGLE_OPEN, ang_close->level);
      return(chunk_get_prev_nnl(pc));
   }
   return(ang_close);
}


/**
 * If attr is CT_ATTRIBUTE, then skip it and the parens and return the chunk
 * after the CT_FPAREN_CLOSE.
 * If the chunk isn't an CT_ATTRIBUTE, then it is returned.
 */
chunk_t *skip_attribute_next(chunk_t *attr)
{
   if ((attr != NULL) && (attr->type == CT_ATTRIBUTE))
   {
      chunk_t *pc = chunk_get_next(attr);
      if ((pc != NULL) && (pc->type == CT_FPAREN_OPEN))
      {
         pc = chunk_get_next_type(attr, CT_FPAREN_CLOSE, attr->level);
         return(chunk_get_next_nnl(pc));
      }
      return(pc);
   }
   return(attr);
}


/**
 * If fp_close is a CT_FPAREN_CLOSE with a parent of CT_ATTRIBUTE, then skip it
 * and the '__attribute__' thingy and return the chunk before CT_ATTRIBUTE.
 * Otherwise return fp_close.
 */
chunk_t *skip_attribute_prev(chunk_t *fp_close)
{
   if ((fp_close != NULL) &&
       (fp_close->type == CT_FPAREN_CLOSE) &&
       (fp_close->parent_type == CT_ATTRIBUTE))
   {
      chunk_t *pc;
      pc = chunk_get_prev_type(fp_close, CT_ATTRIBUTE, fp_close->level);
      return(chunk_get_prev_nnl(pc));
   }
   return(fp_close);
}


/**
 * Process an ObjC 'class'
 * pc is the chunk after '@implementation' or '@interface' or '@protocol'.
 * Change colons, etc. Processes stuff until '@end'.
 * Skips anything in braces.
 */
static void handle_oc_class(chunk_t *pc)
{
   chunk_t *tmp;
   bool    hit_scope = false;
   int     do_pl     = 1;

   LOG_FMT(LOCCLASS, "%s: start [%s] [%s] line %d\n", __func__,
           pc->str.c_str(), get_token_name(pc->parent_type), pc->orig_line);

   if (pc->parent_type == CT_OC_PROTOCOL)
   {
      tmp = chunk_get_next_nnl(pc);
      if (chunk_is_semicolon(tmp))
      {
         tmp->parent_type = pc->parent_type;
         LOG_FMT(LOCCLASS, "%s:   bail on semicolon\n", __func__);
         return;
      }
   }

   tmp = pc;
   while ((tmp = chunk_get_next_nnl(tmp)) != NULL)
   {
      LOG_FMT(LOCCLASS, "%s:       %d [%s]\n", __func__,
              tmp->orig_line, tmp->str.c_str());

      if (tmp->type == CT_OC_END)
      {
         break;
      }
      if ((do_pl == 1) && chunk_is_str(tmp, "<", 1))
      {
         tmp->type        = CT_ANGLE_OPEN;
         tmp->parent_type = CT_OC_PROTO_LIST;
         do_pl            = 2;
      }
      if ((do_pl == 2) && chunk_is_str(tmp, ">", 1))
      {
         tmp->type        = CT_ANGLE_CLOSE;
         tmp->parent_type = CT_OC_PROTO_LIST;
         do_pl            = 0;
      }
      if (tmp->type == CT_BRACE_OPEN)
      {
         do_pl            = 0;
         tmp->parent_type = CT_OC_CLASS;
         tmp = chunk_get_next_type(tmp, CT_BRACE_CLOSE, tmp->level);
         if (tmp != NULL)
         {
            tmp->parent_type = CT_OC_CLASS;
         }
      }
      else if (tmp->type == CT_COLON)
      {
         tmp->type = hit_scope ? CT_OC_COLON : CT_CLASS_COLON;
         if (tmp->type == CT_CLASS_COLON)
         {
            tmp->parent_type = CT_OC_CLASS;
         }
      }
      else if (chunk_is_str(tmp, "-", 1) || chunk_is_str(tmp, "+", 1))
      {
         do_pl = 0;
         if (chunk_is_newline(chunk_get_prev(tmp)))
         {
            tmp->type   = CT_OC_SCOPE;
            tmp->flags |= PCF_STMT_START;
            hit_scope   = true;
         }
      }
      if (do_pl == 2)
      {
         tmp->parent_type = CT_OC_PROTO_LIST;
      }
   }

   if ((tmp != NULL) && (tmp->type == CT_BRACE_OPEN))
   {
      tmp = chunk_get_next_type(tmp, CT_BRACE_CLOSE, tmp->level);
      if (tmp != NULL)
      {
         tmp->parent_type = CT_OC_CLASS;
      }
   }
}


/* Mark Objective-C blocks (aka lambdas or closures)
 *  The syntax and usage is exactly like C function pointers
 *  but instead of an asterisk they have a caret as pointer symbol.
 *  Although it may look expensive this functions is only triggered
 *  on appearance of an OC_BLOCK_CARET for LANG_OC.
 *  repeat(10, ^{ putc('0'+d); });
 *  typedef void (^workBlk_t)(void);
 *
 * @param pc points to the '^'
 */
static void handle_oc_block_literal(fp_data& fpd, chunk_t *pc)
{
   chunk_t *tmp  = pc;
   chunk_t *prev = chunk_get_prev_nnl(pc);
   chunk_t *next = chunk_get_next_nnl(pc);

   if (!pc || !prev || !next)
   {
      return; /* let's be paranoid */
   }

   chunk_t *apo;  /* arg paren open */
   chunk_t *apc;  /* arg paren close */
   chunk_t *bbo;  /* block brace open */
   chunk_t *bbc;  /* block brace close */

   /* block literal: '^ RTYPE ( ARGS ) { }'
    * RTYPE and ARGS are optional
    */
   LOG_FMT(LOCBLK, "%s: block literal @ %d:%d\n", __func__, pc->orig_line, pc->orig_col);

   apo = NULL;
   bbo = NULL;

   LOG_FMT(LOCBLK, "%s:  + scan", __func__);
   for (tmp = next; tmp; tmp = chunk_get_next_nnl(tmp))
   {
      LOG_FMT(LOCBLK, " %s", tmp->text());
      if ((tmp->level < pc->level) || (tmp->type == CT_SEMICOLON))
      {
         LOG_FMT(LOCBLK, "[DONE]");
         break;
      }
      if (tmp->level == pc->level)
      {
         if (chunk_is_paren_open(tmp))
         {
            apo = tmp;
            LOG_FMT(LOCBLK, "[PAREN]");
         }
         if (tmp->type == CT_BRACE_OPEN)
         {
            LOG_FMT(LOCBLK, "[BRACE]");
            bbo = tmp;
            break;
         }
      }
   }

   /* make sure we have braces */
   bbc = chunk_skip_to_match(bbo);
   if (!bbo || !bbc)
   {
      LOG_FMT(LOCBLK, " -- no braces found\n");
      return;
   }
   LOG_FMT(LOCBLK, "\n");

   /* we are on a block literal for sure */
   pc->type        = CT_OC_BLOCK_CARET;
   pc->parent_type = CT_OC_BLOCK_EXPR;

   /* handle the optional args */
   chunk_t *lbp; /* last before paren - end of return type, if any */
   if (apo)
   {
      apc = chunk_skip_to_match(apo);
      if (chunk_is_paren_close(apc))
      {
         LOG_FMT(LOCBLK, " -- marking parens @ %d:%d and %d:%d\n",
                 apo->orig_line, apo->orig_col, apc->orig_line, apc->orig_col);
         flag_parens(apo, PCF_OC_ATYPE, CT_FPAREN_OPEN, CT_OC_BLOCK_EXPR, true);
         fix_fcn_def_params(fpd, apo);
      }
      lbp = chunk_get_prev_nnl(apo);
   }
   else
   {
      lbp = chunk_get_prev_nnl(bbo);
   }

   /* mark the return type, if any */
   while (lbp != pc)
   {
      LOG_FMT(LOCBLK, " -- lbp %s[%s]\n", lbp->text(), get_token_name(lbp->type));
      make_type(lbp);
      lbp->flags      |= PCF_OC_RTYPE;
      lbp->parent_type = CT_OC_BLOCK_EXPR;
      lbp = chunk_get_prev_nnl(lbp);
   }
   /* mark the braces */
   bbo->parent_type = CT_OC_BLOCK_EXPR;
   bbc->parent_type = CT_OC_BLOCK_EXPR;
}


/**
 * Mark Objective-C block types.
 * The syntax and usage is exactly like C function pointers
 * but instead of an asterisk they have a caret as pointer symbol.
 *  typedef void (^workBlk_t)(void);
 *  const char * (^workVar)(void);
 *  -(void)Foo:(void(^)())blk { }
 *
 * This is triggered when the sequence '(' '^' is found.
 *
 * @param pc points to the '^'
 */
static void handle_oc_block_type(fp_data& fpd, chunk_t *pc)
{
   if (!pc)
   {
      return;
   }

   if (pc->flags & PCF_IN_TYPEDEF)
   {
      LOG_FMT(LOCBLK, "%s: skip block type @ %d:%d -- in typedef\n",
              __func__, pc->orig_line, pc->orig_col);
      return;
   }

   chunk_t *tpo;  /* type paren open */
   chunk_t *tpc;  /* type paren close */
   chunk_t *nam;  /* name (if any) of '^' */
   chunk_t *apo;  /* arg paren open */
   chunk_t *apc;  /* arg paren close */

   /* make sure we have '( ^' */
   tpo = chunk_get_prev_nnl(pc);
   if (chunk_is_paren_open(tpo))
   {
      /* block type: 'RTYPE (^LABEL)(ARGS)'
       * LABEL is optional.
       */
      tpc = chunk_skip_to_match(tpo);  /* type close paren (after '^') */
      nam = chunk_get_prev_nnl(tpc);  /* name (if any) or '^' */
      apo = chunk_get_next_nnl(tpc);  /* arg open paren */
      apc = chunk_skip_to_match(apo);  /* arg close paren */

      if (chunk_is_paren_close(apc))
      {
         chunk_t   *aft = chunk_get_next_nnl(apc);
         c_token_t pt;

         if (chunk_is_str(nam, "^", 1))
         {
            nam->type = CT_PTR_TYPE;
            pt        = CT_FUNC_TYPE;
         }
         else if (chunk_is_token(aft, CT_ASSIGN) || chunk_is_token(aft, CT_SEMICOLON))
         {
            nam->type = CT_FUNC_VAR;
            pt        = CT_FUNC_VAR;
         }
         else
         {
            nam->type = CT_FUNC_TYPE;
            pt        = CT_FUNC_TYPE;
         }
         LOG_FMT(LOCBLK, "%s: block type @ %d:%d (%s)[%s]\n", __func__,
                 pc->orig_line, pc->orig_col, nam->text(), get_token_name(nam->type));
         pc->type         = CT_PTR_TYPE;
         pc->parent_type  = pt; //CT_OC_BLOCK_TYPE;
         tpo->type        = CT_TPAREN_OPEN;
         tpo->parent_type = pt; //CT_OC_BLOCK_TYPE;
         tpc->type        = CT_TPAREN_CLOSE;
         tpc->parent_type = pt; //CT_OC_BLOCK_TYPE;
         apo->type        = CT_FPAREN_OPEN;
         apo->parent_type = CT_FUNC_PROTO;
         apc->type        = CT_FPAREN_CLOSE;
         apc->parent_type = CT_FUNC_PROTO;
         fix_fcn_def_params(fpd, apo);
         mark_function_return_type(nam, chunk_get_prev_nnl(tpo), pt);
      }
   }
}


/**
 * Process a type that is enclosed in parens in message decls.
 * TODO: handle block types, which get special formatting
 *
 * @param pc points to the open paren
 * @return the chunk after the type
 */
static chunk_t *handle_oc_md_type(chunk_t *paren_open, c_token_t ptype, UINT64 flags, bool& did_it)
{
   chunk_t *paren_close;

   if (!chunk_is_paren_open(paren_open) ||
       ((paren_close = chunk_skip_to_match(paren_open)) == NULL))
   {
      did_it = false;
      return paren_open;
   }

   did_it = true;

   paren_open->parent_type  = ptype;
   paren_open->flags       |= flags;
   paren_close->parent_type = ptype;
   paren_close->flags      |= flags;

   for (chunk_t *cur = chunk_get_next_nnl(paren_open);
        cur != paren_close;
        cur = chunk_get_next_nnl(cur))
   {
      LOG_FMT(LOCMSGD, " <%s|%s>", cur->text(), get_token_name(cur->type));
      cur->flags |= flags;
      make_type(cur);
   }

   /* returning the chunk after the paren close */
   return chunk_get_next_nnl(paren_close);
}


/**
 * Process an ObjC message spec/dec
 *
 * Specs:
 * -(void) foo ARGS;
 *
 * Decl:
 * -(void) foo ARGS {  }
 *
 * LABEL : (ARGTYPE) ARGNAME
 *
 * ARGS is ': (ARGTYPE) ARGNAME [MOREARGS...]'
 * MOREARGS is ' [ LABEL] : (ARGTYPE) ARGNAME '
 * -(void) foo: (int) arg: {  }
 * -(void) foo: (int) arg: {  }
 * -(void) insertObject:(id)anObject atIndex:(int)index
 */
static void handle_oc_message_decl(chunk_t *pc)
{
   chunk_t   *tmp;
   bool      in_paren  = false;
   int       paren_cnt = 0;
   int       arg_cnt   = 0;
   c_token_t pt;
   bool      did_it;

   /* Figure out if this is a spec or decl */
   tmp = pc;
   while ((tmp = chunk_get_next(tmp)) != NULL)
   {
      if (tmp->level < pc->level)
      {
         /* should not happen */
         return;
      }
      if ((tmp->type == CT_SEMICOLON) ||
          (tmp->type == CT_BRACE_OPEN))
      {
         break;
      }
   }
   if (tmp == NULL)
   {
      return;
   }
   pt = (tmp->type == CT_SEMICOLON) ? CT_OC_MSG_SPEC : CT_OC_MSG_DECL;

   pc->type        = CT_OC_SCOPE;
   pc->parent_type = pt;

   LOG_FMT(LOCMSGD, "%s: %s @ %d:%d -", __func__, get_token_name(pt), pc->orig_line, pc->orig_col);

   /* format: -(TYPE) NAME [: (TYPE)NAME */

   /* handle the return type */
   tmp = handle_oc_md_type(chunk_get_next_nnl(pc), pt, PCF_OC_RTYPE, did_it);
   if (!did_it)
   {
      LOG_FMT(LOCMSGD, " -- missing type parens\n");
      return;
   }

   /* expect the method name/label */
   if (!chunk_is_token(tmp, CT_WORD))
   {
      LOG_FMT(LOCMSGD, " -- missing method name\n");
      return;
   }

   chunk_t *label = tmp;
   tmp->type = pt;
   tmp->parent_type = pt;
   pc = chunk_get_next_nnl(tmp);

   LOG_FMT(LOCMSGD, " [%s]%s", pc->text(), get_token_name(pc->type));

   /* if we have a colon next, we have args */
   if ((pc->type == CT_COLON) || (pc->type == CT_OC_COLON))
   {
      pc = label;

      while (true)
      {
         /* skip optional label */
         if (chunk_is_token(pc, CT_WORD) || chunk_is_token(pc, pt))
         {
            pc->parent_type = pt;
            pc = chunk_get_next_nnl(pc);
         }
         /* a colon must be next */
         if (!chunk_is_str(pc, ":", 1))
         {
            break;
         }
         pc->type        = CT_OC_COLON;
         pc->parent_type = pt;
         pc = chunk_get_next_nnl(pc);

         /* next is the type in parens */
         LOG_FMT(LOCMSGD, "  (%s)", pc->text());
         tmp = handle_oc_md_type(pc, pt, PCF_OC_ATYPE, did_it);
         if (!did_it)
         {
            LOG_FMT(LWARN, "%s: %d:%d expected type\n", __func__, pc->orig_line, pc->orig_col);
            break;
         }
         pc = tmp;
         /* we should now be on the arg name */
         pc->flags |= PCF_VAR_DEF;
         LOG_FMT(LOCMSGD, " arg[%s]", pc->text());
         pc = chunk_get_next_nnl(pc);
      }
   }

   LOG_FMT(LOCMSGD, " end[%s]", pc->text());

   if (chunk_is_token(pc, CT_BRACE_OPEN))
   {
      pc->parent_type = pt;
      pc = chunk_skip_to_match(pc);
      if (pc)
      {
         pc->parent_type = pt;
      }
   }
   else if (chunk_is_token(pc, CT_SEMICOLON))
   {
      pc->parent_type = pt;
   }

   LOG_FMT(LOCMSGD, "\n");
   return;

   /* Mark everything */
   tmp = pc;
   while ((tmp = chunk_get_next(tmp)) != NULL)
   {
      LOG_FMT(LOCMSGD, " [%s]", tmp->text());

      if ((tmp->type == CT_SEMICOLON) ||
          (tmp->type == CT_BRACE_OPEN))
      {
         tmp->parent_type = pt;
         break;
      }

      /* Mark first parens as return type */
      if ((arg_cnt == 0) &&
          ((tmp->type == CT_PAREN_OPEN) ||
           (tmp->type == CT_PAREN_CLOSE)))
      {
         tmp->parent_type = CT_OC_RTYPE;
         in_paren         = (tmp->type == CT_PAREN_OPEN);
         if (!in_paren)
         {
            paren_cnt++;
            arg_cnt++;
         }
      }
      else if ((tmp->type == CT_PAREN_OPEN) ||
               (tmp->type == CT_PAREN_CLOSE))
      {
         tmp->parent_type = pt;
         in_paren         = (tmp->type == CT_PAREN_OPEN);
         if (!in_paren)
         {
            paren_cnt++;
         }
      }
      else if (tmp->type == CT_WORD)
      {
         if (in_paren)
         {
            tmp->type        = CT_TYPE;
            tmp->parent_type = pt;
         }
         else if (paren_cnt == 1)
         {
            tmp->type = pt;
         }
         else
         {
            tmp->flags |= PCF_VAR_DEF;
         }
      }
      else if (tmp->type == CT_COLON)
      {
         tmp->type        = CT_OC_COLON;
         tmp->parent_type = pt;
      }
   }

   if ((tmp != NULL) && (tmp->type == CT_BRACE_OPEN))
   {
      tmp = chunk_skip_to_match(tmp);
      if (tmp)
      {
         tmp->parent_type = pt;
      }
   }
   LOG_FMT(LOCMSGD, "\n");
}


/**
 * Process an ObjC message send statement:
 * [ class func: val1 name2: val2 name3: val3] ; // named params
 * [ class func: val1      : val2      : val3] ; // unnamed params
 * [ class <proto> self method ] ; // with protocol
 * [[NSMutableString alloc] initWithString: @"" ] // class from msg
 * [func(a,b,c) lastObject ] // class from func
 *
 * Mainly find the matching ']' and ';' and mark the colons.
 *
 * @param os points to the open square '['
 */
static void handle_oc_message_send(chunk_t *os)
{
   chunk_t *tmp;
   chunk_t *cs = chunk_get_next(os);

   while ((cs != NULL) && (cs->level > os->level))
   {
      cs = chunk_get_next(cs);
   }

   if ((cs == NULL) || (cs->type != CT_SQUARE_CLOSE))
   {
      return;
   }

   LOG_FMT(LOCMSG, "%s: line %d, col %d\n", __func__, os->orig_line, os->orig_col);

   tmp = chunk_get_next_nnl(cs);
   if (chunk_is_semicolon(tmp))
   {
      tmp->parent_type = CT_OC_MSG;
   }

   os->parent_type = CT_OC_MSG;
   os->flags      |= PCF_IN_OC_MSG;
   cs->parent_type = CT_OC_MSG;
   cs->flags      |= PCF_IN_OC_MSG;

   /* expect a word first thing or [...] */
   tmp = chunk_get_next_nnl(os);
   if (tmp->type == CT_SQUARE_OPEN)
   {
      tmp = chunk_skip_to_match(tmp);
   }
   else if ((tmp->type != CT_WORD) && (tmp->type != CT_TYPE))
   {
      LOG_FMT(LOCMSG, "%s: %d:%d expected identifier, not '%s' [%s]\n", __func__,
              tmp->orig_line, tmp->orig_col,
              tmp->text(), get_token_name(tmp->type));
      return;
   }
   else
   {
      chunk_t *tt = chunk_get_next_nnl(tmp);
      if (chunk_is_paren_open(tt))
      {
         tmp->type = CT_FUNC_CALL;
         tmp = chunk_get_prev_nnl(set_paren_parent(tt, CT_FUNC_CALL));
      }
      else
      {
         tmp->type = CT_OC_MSG_CLASS;
      }
   }

   /* handle '< protocol >' */
   tmp = chunk_get_next_nnl(tmp);
   if (chunk_is_str(tmp, "<", 1))
   {
      chunk_t *ao = tmp;
      chunk_t *ac = chunk_get_next_str(ao, ">", 1, ao->level);

      if (ac)
      {
         ao->type = CT_ANGLE_OPEN;
         ao->parent_type = CT_OC_PROTO_LIST;
         ac->type = CT_ANGLE_CLOSE;
         ac->parent_type = CT_OC_PROTO_LIST;
         for (tmp = chunk_get_next(ao); tmp != ac; tmp = chunk_get_next(tmp))
         {
            tmp->level += 1;
            tmp->parent_type = CT_OC_PROTO_LIST;
         }
      }
      tmp = chunk_get_next_nnl(ac);
   }

   if (tmp && ((tmp->type == CT_WORD) || (tmp->type == CT_TYPE)))
   {
      tmp->type = CT_OC_MSG_FUNC;
   }

   chunk_t *prev = NULL;

   for (tmp = chunk_get_next(os); tmp != cs; tmp = chunk_get_next(tmp))
   {
      tmp->flags |= PCF_IN_OC_MSG;
      if (tmp->level == cs->level + 1)
      {
         if (tmp->type == CT_COLON)
         {
            tmp->type = CT_OC_COLON;
            if ((prev != NULL) && ((prev->type == CT_WORD) || (prev->type == CT_TYPE)))
            {
               /* Might be a named param, check previous block */
               chunk_t *pp = chunk_get_prev(prev);
               if ((pp != NULL) &&
                   (pp->type != CT_OC_COLON) &&
                   (pp->type != CT_ARITH) &&
                   (pp->type != CT_CARET))
               {
                  prev->type       = CT_OC_MSG_NAME;
                  tmp->parent_type = CT_OC_MSG_NAME;
               }
            }
         }
      }
      prev = tmp;
   }
}


/**
 * Process an C# [] thingy:
 *    [assembly: xxx]
 *    [AttributeUsage()]
 *    [@X]
 *
 * Set the next chunk to a statement start after the close ']'
 *
 * @param os points to the open square '['
 */
static void handle_cs_square_stmt(chunk_t *os)
{
   chunk_t *tmp;
   chunk_t *cs = chunk_get_next(os);

   while ((cs != NULL) && (cs->level > os->level))
   {
      cs = chunk_get_next(cs);
   }

   if ((cs == NULL) || (cs->type != CT_SQUARE_CLOSE))
   {
      return;
   }

   os->parent_type = CT_CS_SQ_STMT;
   cs->parent_type = CT_CS_SQ_STMT;

   for (tmp = chunk_get_next(os); tmp != cs; tmp = chunk_get_next(tmp))
   {
      tmp->parent_type = CT_CS_SQ_STMT;
      if (tmp->type == CT_COLON)
      {
         tmp->type = CT_CS_SQ_COLON;
      }
   }

   tmp = chunk_get_next_nnl(cs);
   if (tmp != NULL)
   {
      tmp->flags |= PCF_STMT_START | PCF_EXPR_START;
   }
}


/**
 * We are on a brace open that is preceded by a word or square close.
 * Set the brace parent to CT_CS_PROPERTY and find the first item in the
 * property and set its parent, too.
 */
static void handle_cs_property(chunk_t *bro)
{
   chunk_t *pc;
   bool    did_prop = false;

   set_paren_parent(bro, CT_CS_PROPERTY);

   pc = bro;
   while ((pc = chunk_get_prev_nnl(pc)) != NULL)
   {
      if (pc->level == bro->level)
      {
         if (!did_prop && ((pc->type == CT_WORD) || (pc->type == CT_THIS)))
         {
            pc->type = CT_CS_PROPERTY;
            did_prop = true;
         }
         else
         {
            pc->parent_type = CT_CS_PROPERTY;
            make_type(pc);
         }
         if (pc->flags & PCF_STMT_START)
         {
            break;
         }
      }
   }
}


/**
 * A func wrap chunk and what follows should be treated as a function name.
 * Create new text for the chunk and call it a CT_FUNCTION.
 *
 * A type wrap chunk and what follows should be treated as a simple type.
 * Create new text for the chunk and call it a CT_TYPE.
 */
static void handle_wrap(fp_data& fpd, chunk_t *pc)
{
   chunk_t *opp  = chunk_get_next(pc);
   chunk_t *name = chunk_get_next(opp);
   chunk_t *clp  = chunk_get_next(name);

   if ((clp != NULL) &&
       (opp->type == CT_PAREN_OPEN) &&
       ((name->type == CT_WORD) || (name->type == CT_TYPE)) &&
       (clp->type == CT_PAREN_CLOSE))
   {
      pc->str.append("(");
      pc->str.append(name->str);
      pc->str.append(")");

      pc->type = (pc->type == CT_FUNC_WRAP) ? CT_FUNCTION : CT_TYPE;

      pc->orig_col_end = pc->orig_col + pc->len();

      chunk_del(fpd, opp);
      chunk_del(fpd, name);
      chunk_del(fpd, clp);
   }
}


/**
 * A proto wrap chunk and what follows should be treated as a function proto.
 *
 * RETTYPE PROTO_WRAP( NAME, PARAMS );
 * RETTYPE gets changed with make_type().
 * PROTO_WRAP is marked as CT_FUNC_PROTO or CT_FUNC_DEF.
 * NAME is marked as CT_WORD.
 * PARAMS is all marked as prototype parameters.
 */
static void handle_proto_wrap(fp_data& fpd, chunk_t *pc)
{
   chunk_t *opp  = chunk_get_next_nnl(pc);
   chunk_t *name = chunk_get_next_nnl(opp);
   chunk_t *tmp  = chunk_get_next_nnl(chunk_get_next_nnl(name));
   chunk_t *clp  = chunk_skip_to_match(opp);
   chunk_t *cma  = chunk_get_next_nnl(clp);

   if (!opp || !name || !clp || !cma || !tmp ||
       ((name->type != CT_WORD) && (name->type != CT_TYPE)) ||
       (tmp->type != CT_PAREN_OPEN) ||
       (opp->type != CT_PAREN_OPEN))
   {
      return;
   }
   if (cma->type == CT_SEMICOLON)
   {
      pc->type = CT_FUNC_PROTO;
   }
   else if (cma->type == CT_BRACE_OPEN)
   {
      pc->type = CT_FUNC_DEF;
   }
   else
   {
      return;
   }
   opp->parent_type = pc->type;
   clp->parent_type = pc->type;

   tmp->parent_type = CT_PROTO_WRAP;
   fix_fcn_def_params(fpd, tmp);
   tmp = chunk_skip_to_match(tmp);
   if (tmp)
   {
      tmp->parent_type = CT_PROTO_WRAP;
   }

   /* Mark return type (TODO: move to own function) */
   tmp = pc;
   while ((tmp = chunk_get_prev_nnl(tmp)) != NULL)
   {
      if (!chunk_is_type(tmp) &&
          (tmp->type != CT_OPERATOR) &&
          (tmp->type != CT_WORD) &&
          (tmp->type != CT_ADDR))
      {
         break;
      }
      tmp->parent_type = pc->type;
      make_type(tmp);
   }
}


/**
 * Java assert statments are: "assert EXP1 [: EXP2] ;"
 * Mark the parent of the colon and semicolon
 */
static void handle_java_assert(chunk_t *pc)
{
   bool    did_colon = false;
   chunk_t *tmp      = pc;

   while ((tmp = chunk_get_next(tmp)) != NULL)
   {
      if (tmp->level == pc->level)
      {
         if (!did_colon && (tmp->type == CT_COLON))
         {
            did_colon        = true;
            tmp->parent_type = pc->type;
         }
         if (tmp->type == CT_SEMICOLON)
         {
            tmp->parent_type = pc->type;
            break;
         }
      }
   }
}
