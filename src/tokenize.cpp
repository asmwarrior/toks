/**
 * @file tokenize.cpp
 * This file breaks up the text stream into tokens or chunks.
 *
 * Each routine needs to set pc.len and pc.type.
 *
 * @author  Ben Gardner
 * @license GPL v2+
 */
#include "toks_types.h"
#include "char_table.h"
#include "prototypes.h"
#include "chunk_list.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cctype>

struct tok_info
{
   tok_info() : last_ch(0), idx(0), row(1), col(1)
   {
   }
   int last_ch;
   int idx;
   int row;
   int col;
};

struct tok_ctx
{
   tok_ctx(const vector<UINT8>& d) : data(d)
   {
   }

   /* save before trying to parse something that may fail */
   void save()
   {
      save(s);
   }
   void save(tok_info& info)
   {
      info = c;
   }

   /* restore previous saved state */
   void restore()
   {
      restore(s);
   }
   void restore(const tok_info& info)
   {
      c = info;
   }

   bool more()
   {
      return(c.idx < (int)data.size());
   }

   int peek()
   {
      return(more() ? data[c.idx] : -1);
   }

   int peek(int idx)
   {
      idx += c.idx;
      return((idx < (int)data.size()) ? data[idx] : -1);
   }

   int get()
   {
      if (more())
      {
         int ch = data[c.idx++];
         switch (ch)
         {
         case '\t':
            c.col = calc_next_tab_column(c.col, UO_input_tab_size);
            break;

         case '\n':
            if (c.last_ch != '\r')
            {
               c.row++;
               c.col = 1;
            }
            break;

         case '\r':
            c.row++;
            c.col = 1;
            break;

         default:
            /* skip continuation bytes in surrogate pairs */
            if ((ch & 0xC0) != 0x80)
               c.col++;
            break;
         }
         c.last_ch = ch;
         return ch;
      }
      return -1;
   }

   bool expect(int ch)
   {
      if (peek() == ch)
      {
         get();
         return true;
      }
      return false;
   }

   const vector<UINT8>& data;
   tok_info          c; /* current */
   tok_info          s; /* saved */
};

static bool parse_string(tok_ctx& ctx, chunk_t& pc, int quote_idx, bool allow_escape);


/**
 * Parses all legal D string constants.
 *
 * Quoted strings:
 *   r"Wysiwyg"      # WYSIWYG string
 *   x"hexstring"    # Hexadecimal array
 *   `Wysiwyg`       # WYSIWYG string
 *   'char'          # single character
 *   "reg_string"    # regular string
 *
 * Non-quoted strings:
 * \x12              # 1-byte hex constant
 * \u1234            # 2-byte hex constant
 * \U12345678        # 4-byte hex constant
 * \123              # octal constant
 * \&amp;            # named entity
 * \n                # single character
 *
 * @param pc   The structure to update, str is an input.
 * @return     Whether a string was parsed
 */
static bool d_parse_string(tok_ctx& ctx, chunk_t& pc)
{
   int ch = ctx.peek();

   if ((ch == '"') || (ch == '\'') || (ch == '`'))
   {
      return(parse_string(ctx, pc, 0, true));
   }
   else if (ch == '\\')
   {
      ctx.save();
      int cnt;
      pc.str.clear();
      while (ctx.peek() == '\\')
      {
         pc.str.append(1, ctx.get());
         /* Check for end of file */
         switch (ctx.peek())
         {
         case 'x':
            /* \x HexDigit HexDigit */
            cnt = 3;
            while (cnt--)
            {
               pc.str.append(1, ctx.get());
            }
            break;

         case 'u':
            /* \u HexDigit HexDigit HexDigit HexDigit */
            cnt = 5;
            while (cnt--)
            {
               pc.str.append(1, ctx.get());
            }
            break;

         case 'U':
            /* \U HexDigit (x8) */
            cnt = 9;
            while (cnt--)
            {
               pc.str.append(1, ctx.get());
            }
            break;

         case '0':
         case '1':
         case '2':
         case '3':
         case '4':
         case '5':
         case '6':
         case '7':
            /* handle up to 3 octal digits */
            pc.str.append(1, ctx.get());
            ch = ctx.peek();
            if ((ch >= '0') && (ch <= '7'))
            {
               pc.str.append(1, ctx.get());
               ch = ctx.peek();
               if ((ch >= '0') && (ch <= '7'))
               {
                  pc.str.append(1, ctx.get());
               }
            }
            break;

         case '&':
            /* \& NamedCharacterEntity ; */
            pc.str.append(1, ctx.get());
            while (isalpha(ctx.peek()))
            {
               pc.str.append(1, ctx.get());
            }
            if (ctx.peek() == ';')
            {
               pc.str.append(1, ctx.get());
            }
            break;

         default:
            /* Everything else is a single character */
            pc.str.append(1, ctx.get());
            break;
         }
      }

      if (pc.str.size() > 1)
      {
         pc.type = CT_STRING;
         return(true);
      }
      ctx.restore();
   }
   else if (((ch == 'r') || (ch == 'x')) && (ctx.peek(1) == '"'))
   {
      return(parse_string(ctx, pc, 1, false));
   }
   return(false);
}


// /**
//  * A string-in-string search.  Like strstr() with a haystack length.
//  */
// static const char *str_search(const char *needle, const char *haystack, int haystack_len)
// {
//    int needle_len = strlen(needle);
//
//    while (haystack_len-- >= needle_len)
//    {
//       if (memcmp(needle, haystack, needle_len) == 0)
//       {
//          return(haystack);
//       }
//       haystack++;
//    }
//    return(NULL);
// }


/**
 * Figure of the length of the comment at text.
 * The next bit of text starts with a '/', so it might be a comment.
 * There are three types of comments:
 *  - C comments that start with  '/ *' and end with '* /'
 *  - C++ comments that start with //
 *  - D nestable comments '/+' '+/'
 *
 * @param pc   The structure to update, str is an input.
 * @return     Whether a comment was parsed
 */
static bool parse_comment(fp_data& fpd, tok_ctx& ctx, chunk_t& pc)
{
   int  ch;
   bool is_d    = (fpd.lang_flags & LANG_D) != 0;
   int  d_level = 0;
   int  bs_cnt;

   ch = ctx.peek(1);

   /* does this start with '/ /' or '/ *' or '/ +' (d) */
   if ((ctx.peek() != '/') ||
       ((ch != '*') && (ch != '/') &&
        ((ch != '+') || !is_d)))
   {
      return(false);
   }

   ctx.save();

   /* account for opening two chars */
   (void) ctx.get();   /* opening '/' */
   (void) ctx.get();   /* second char */

   if (ch == '/')
   {
      pc.type = CT_WHITESPACE;
      while (true)
      {
         bs_cnt = 0;
         while ((ch = ctx.peek()) >= 0)
         {
            if ((ch == '\r') || (ch == '\n'))
            {
               break;
            }
            if (ch == '\\')
            {
               bs_cnt++;
            }
            else
            {
               bs_cnt = 0;
            }
            (void) ctx.get();
         }

         /* If we hit an odd number of backslashes right before the newline,
          * then we keep going.
          */
         if (((bs_cnt & 1) == 0) || !ctx.more())
         {
            break;
         }
         if (ctx.peek() == '\r')
         {
            (void) ctx.get();
         }
         if (ctx.peek() == '\n')
         {
            (void) ctx.get();
         }
      }
   }
   else if (!ctx.more())
   {
      /* unexpected end of file */
      ctx.restore();
      return(false);
   }
   else if (ch == '*')
   {
      pc.type = CT_WHITESPACE;
      while ((ch = ctx.get()) >= 0)
      {
         if (ch == '*' && ctx.peek() == '/')
         {
            (void) ctx.get(); /* discard the '/' */
            break;
         }
      }
   }
   else /* must be '/ +' */
   {
      pc.type = CT_WHITESPACE;
      d_level++;
      while ((d_level > 0) && ctx.more())
      {
         if ((ctx.peek() == '+') && (ctx.peek(1) == '/'))
         {
            (void) ctx.get();  /* discard the '+' */
            (void) ctx.get();  /* discard the '/' */
            d_level--;
            continue;
         }

         if ((ctx.peek() == '/') && (ctx.peek(1) == '+'))
         {
            (void) ctx.get();  /* discard the '/' */
            (void) ctx.get();  /* discard the '+' */
            d_level++;
            continue;
         }

         ch = ctx.get();
         if (ch == '\r')
         {
            if (ctx.peek() == '\n')
            {
               (void) ctx.get();  /* discard the '\n' */
            }
         }
      }
   }

   return(true);
}


/**
 * Parse any attached suffix, which may be a user-defined literal suffix.
 * If for a string, explicitly exclude common format and scan specifiers, ie,
 * PRIx32 and SCNx64.
 */
static void parse_suffix(tok_ctx& ctx, chunk_t& pc, bool forstring = false)
{
   if (CharTable::IsKw1(ctx.peek()))
   {
      int slen = 0;
      int oldsize = pc.str.size();
      tok_info ss;

      /* don't add the suffix if we see L" or L' or S" */
      int p1 = ctx.peek();
      int p2 = ctx.peek(1);
      if (forstring &&
          (((p1 == 'L') && ((p2 == '"') || (p2 == '\''))) ||
           ((p1 == 'S') && (p2 == '"'))))
      {
          return;
      }
      ctx.save(ss);
      while (ctx.more() && CharTable::IsKw2(ctx.peek()))
      {
         slen++;
         pc.str.append(1, ctx.get());
      }

      if (forstring && (slen >= 4) &&
          ((pc.str.substr(0, 3) == "PRI") ||
           (pc.str.substr(0, 3) == "SCN")))
      {
         ctx.restore(ss);
         pc.str.resize(oldsize);
      }
   }
}


static bool is_bin(int ch)
{
   return((ch == '0') || (ch == '1'));
}

static bool is_bin_(int ch)
{
   return(is_bin(ch) || (ch == '_'));
}

static bool is_oct(int ch)
{
   return((ch >= '0') && (ch <= '7'));
}

static bool is_oct_(int ch)
{
   return(is_oct(ch) || (ch == '_'));
}

static bool is_dec(int ch)
{
   return((ch >= '0') && (ch <= '9'));
}

static bool is_dec_(int ch)
{
   return(is_dec(ch) || (ch == '_'));
}

static bool is_hex(int ch)
{
   return(((ch >= '0') && (ch <= '9')) ||
          ((ch >= 'a') && (ch <= 'f')) ||
          ((ch >= 'A') && (ch <= 'F')));
}

static bool is_hex_(int ch)
{
   return(is_hex(ch) || (ch == '_'));
}


/**
 * Count the number of characters in the number.
 * The next bit of text starts with a number (0-9 or '.'), so it is a number.
 * Count the number of characters in the number.
 *
 * This should cover all number formats for all languages.
 * Note that this is not a strict parser. It will happily parse numbers in
 * an invalid format.
 *
 * For example, only D allows underscores in the numbers, but they are
 * allowed in all formats.
 *
 * @param pc   The structure to update, str is an input.
 * @return     Whether a number was parsed
 */
static bool parse_number(tok_ctx& ctx, chunk_t& pc)
{
   int  tmp;
   bool is_float;
   bool did_hex = false;

   /* A number must start with a digit or a dot, followed by a digit */
   if (!is_dec(ctx.peek()) &&
       ((ctx.peek() != '.') || !is_dec(ctx.peek(1))))
   {
      return(false);
   }

   is_float = (ctx.peek() == '.');
   if (is_float && (ctx.peek(1) == '.'))
   {
      return(false);
   }

   /* Check for Hex, Octal, or Binary
    * Note that only D and Pawn support binary, but who cares?
    */
   if (ctx.peek() == '0')
   {
      pc.str.append(1, ctx.get());  /* store the '0' */

      switch (toupper(ctx.peek()))
      {
      case 'X':               /* hex */
         did_hex = true;
         do
         {
            pc.str.append(1, ctx.get());  /* store the 'x' and then the rest */
         } while (is_hex_(ctx.peek()));
         break;

      case 'B':               /* binary */
         do
         {
            pc.str.append(1, ctx.get());  /* store the 'b' and then the rest */
         } while (is_bin_(ctx.peek()));
         break;

      case '0':                /* octal or decimal */
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
         do
         {
            pc.str.append(1, ctx.get());
         } while (is_oct_(ctx.peek()));
         break;

      default:
         /* either just 0 or 0.1 or 0UL, etc */
         break;
      }
   }
   else
   {
      /* Regular int or float */
      while (is_dec_(ctx.peek()))
      {
         pc.str.append(1, ctx.get());
      }
   }

   /* Check if we stopped on a decimal point & make sure it isn't '..' */
   if ((ctx.peek() == '.') && (ctx.peek(1) != '.'))
   {
      pc.str.append(1, ctx.get());
      is_float = true;
      if (did_hex)
      {
         while (is_hex_(ctx.peek()))
         {
            pc.str.append(1, ctx.get());
         }
      }
      else
      {
         while (is_dec_(ctx.peek()))
         {
            pc.str.append(1, ctx.get());
         }
      }
   }

   /* Check exponent
    * Valid exponents per language (not that it matters):
    * C/C++/D/Java: eEpP
    * C#/Pawn:      eE
    */
   tmp = toupper(ctx.peek());
   if ((tmp == 'E') || (tmp == 'P'))
   {
      is_float = true;
      pc.str.append(1, ctx.get());
      if ((ctx.peek() == '+') || (ctx.peek() == '-'))
      {
         pc.str.append(1, ctx.get());
      }
      while (is_dec_(ctx.peek()))
      {
         pc.str.append(1, ctx.get());
      }
   }

   /* Check the suffixes
    * Valid suffixes per language (not that it matters):
    *        Integer       Float
    * C/C++: uUlL64        lLfF
    * C#:    uUlL          fFdDMm
    * D:     uUL           ifFL
    * Java:  lL            fFdD
    * Pawn:  (none)        (none)
    *
    * Note that i, f, d, and m only appear in floats.
    */
   while (1)
   {
      tmp = toupper(ctx.peek());
      if ((tmp == 'I') || (tmp == 'F') || (tmp == 'D') || (tmp == 'M'))
      {
         is_float = true;
      }
      else if ((tmp != 'L') && (tmp != 'U'))
      {
         break;
      }
      pc.str.append(1, ctx.get());
   }

   /* skip the Microsoft-specific '64' suffix */
   if ((ctx.peek() == '6') && (ctx.peek(1) == '4'))
   {
      pc.str.append(1, ctx.get());
      pc.str.append(1, ctx.get());
   }

   pc.type = is_float ? CT_NUMBER_FP : CT_NUMBER;

   /* If there is anything left, then we are probably dealing with garbage or
    * some sick macro junk. Eat it.
    */
   parse_suffix(ctx, pc);

   return(true);
}


/**
 * Count the number of characters in a quoted string.
 * The next bit of text starts with a quote char " or ' or <.
 * Count the number of characters until the matching character.
 *
 * @param pc   The structure to update, str is an input.
 * @return     Whether a string was parsed
 */
static bool parse_string(tok_ctx& ctx, chunk_t& pc, int quote_idx, bool allow_escape)
{
   bool escaped = 0;
   int  end_ch;
   char escape_char  = UO_string_escape_char;
   char escape_char2 = UO_string_escape_char2;

   pc.str.clear();
   while (quote_idx-- > 0)
   {
      pc.str.append(1, ctx.get());
   }

   pc.type = CT_STRING;
   end_ch  = CharTable::Get(ctx.peek()) & 0xff;
   pc.str.append(1, ctx.get());  /* store the " */

   while (ctx.more())
   {
      int ch = ctx.get();
      pc.str.append(1, ch);
      if (ch == '\n')
      {
         pc.type = CT_STRING_MULTI;
         escaped = 0;
         continue;
      }
      if ((ch == '\r') && (ctx.peek() != '\n'))
      {
         pc.str.append(1, ctx.get());
         pc.type = CT_STRING_MULTI;
         escaped = 0;
         continue;
      }
      if (!escaped)
      {
         if (ch == escape_char)
         {
            escaped = (escape_char != 0);
         }
         else if ((ch == escape_char2) && (ctx.peek() == end_ch))
         {
            escaped = allow_escape;
         }
         else if (ch == end_ch)
         {
            break;
         }
      }
      else
      {
         escaped = false;
      }
   }

   parse_suffix(ctx, pc, true);
   return(true);
}


/**
 * Literal string, ends with single "
 * Two "" don't end the string.
 *
 * @param pc   The structure to update, str is an input.
 * @return     Whether a string was parsed
 */
static bool parse_cs_string(tok_ctx& ctx, chunk_t& pc)
{
   pc.str = ctx.get();
   pc.str.append(1, ctx.get());

   /* go until we hit a zero (end of file) or a single " */
   while (ctx.more())
   {
      int ch = ctx.get();
      pc.str.append(1, ch);
      if (ch == '"')
      {
         if (ctx.peek() == '"')
         {
            pc.str.append(1, ctx.get());
         }
         else
         {
            break;
         }
      }
   }

   pc.type = CT_STRING;
   return(true);
}


static bool tag_compare(const vector<UINT8>& d, int a_idx, int b_idx, int len)
{
   if (a_idx != b_idx)
   {
      while (len-- > 0)
      {
         if (d[a_idx] != d[b_idx])
         {
            return false;
         }
      }
   }
   return true;
}


/**
 * Parses a C++0x 'R' string. R"( xxx )" R"tag(  )tag" u8R"(x)" uR"(x)"
 * Newlines may be in the string.
 */
static bool parse_cr_string(tok_ctx& ctx, chunk_t& pc, int q_idx)
{
   int cnt;
   int tag_idx = ctx.c.idx + q_idx + 1;
   int tag_len = 0;

   ctx.save();

   /* Copy the prefix + " to the string */
   pc.str.clear();
   cnt = q_idx + 1;
   while (cnt--)
   {
      pc.str.append(1, ctx.get());
   }

   /* Add the tag and get the length of the tag */
   while (ctx.more() && (ctx.peek() != '('))
   {
      tag_len++;
      pc.str.append(1, ctx.get());
   }
   if (ctx.peek() != '(')
   {
      ctx.restore();
      return(false);
   }

   pc.type = CT_STRING;
   while (ctx.more())
   {
      if ((ctx.peek() == ')') &&
          (ctx.peek(tag_len + 1) == '"') &&
          tag_compare(ctx.data, tag_idx, ctx.c.idx + 1, tag_len))
      {
         cnt = tag_len + 2;   /* for the )" */
         while (cnt--)
         {
            pc.str.append(1, ctx.get());
         }
         parse_suffix(ctx, pc);
         return(true);
      }
      if (ctx.peek() == '\n')
      {
         pc.str.append(1, ctx.get());
         pc.type = CT_STRING_MULTI;
      }
      else
      {
         pc.str.append(1, ctx.get());
      }
   }
   ctx.restore();
   return(false);
}


/**
 * Count the number of characters in a word.
 * The first character is already valid for a keyword
 *
 * @param pc   The structure to update, str is an input.
 * @return     Whether a word was parsed (always true)
 */
static bool parse_word(fp_data& fpd, tok_ctx& ctx, chunk_t& pc, bool skipcheck, int preproc_ncnl_count, c_token_t in_preproc)
{
   int             ch;

   /* The first character is already valid */
   pc.str.clear();
   pc.str.append(1, ctx.get());

   while (ctx.more() && CharTable::IsKw2(ctx.peek()))
   {
      ch = ctx.get();
      pc.str.append(1, ch);

      /* HACK: Non-ASCII character are only allowed in identifiers */
      if (ch > 0x7f)
      {
         skipcheck = true;
      }
   }
   pc.type = CT_WORD;

   if (skipcheck)
   {
      return(true);
   }

   /* Detect pre-processor functions now */
   if ((in_preproc == CT_PP_DEFINE) &&
       (preproc_ncnl_count == 1))
   {
      if (ctx.peek() == '(')
      {
         pc.type = CT_MACRO_FUNC;
      }
      else
      {
         pc.type = CT_MACRO;
      }
   }
   else
   {
      /* '@interface' is reserved, not an interface itself */
      if ((fpd.lang_flags & LANG_JAVA) && (pc.str.substr(0, 1) == "@") &&
          (pc.str != "@interface"))
      {
         pc.type = CT_ANNOTATION;
      }
      else
      {
         /* Turn it into a keyword now */
         pc.type = find_keyword_type(pc.str.c_str(), pc.str.size(), in_preproc, fpd.lang_flags);
         if (pc.type != CT_WORD)
         {
             pc.flags |= PCF_KEYWORD;
         }
      }
   }

   return(true);
}


/**
 * Count the number of whitespace characters.
 *
 * @param pc   The structure to update, str is an input.
 * @return     Whether whitespace was parsed
 */
static bool parse_whitespace(tok_ctx& ctx, chunk_t& pc)
{
   bool nl_found = false;
   bool ret = false;

   while (isspace(ctx.peek()))
   {
      if (ctx.get() == '\n')
      {
         nl_found = true;
      }
      ret = true;
   }

   if (ret)
   {
      pc.type = nl_found ? CT_NEWLINE : CT_WHITESPACE;
   }

   return ret;
}


/**
 * Called when we hit a backslash.
 * If there is nothing but whitespace until the newline, then this is a
 * backslash newline
 */
static bool parse_bs_newline(tok_ctx& ctx, chunk_t& pc)
{
   ctx.save();
   ctx.get(); /* skip the '\' */

   int ch;
   while (isspace(ch = ctx.peek()))
   {
      ctx.get();
      if ((ch == '\r') || (ch == '\n'))
      {
         if (ch == '\r')
         {
            ctx.expect('\n');
         }
         pc.str      = "\\";
         pc.type     = CT_NL_CONT;
         return(true);
      }
   }

   ctx.restore();
   return(false);
}


/**
 * Skips the next bit of whatever and returns the type of block.
 *
 * pc.str is the input text.
 * pc.len in the output length.
 * pc.type is the output type
 *
 * @param pc      The structure to update, str is an input.
 * @return        true/false - whether anything was parsed
 */
static bool parse_next(fp_data& fpd, tok_ctx& ctx, chunk_t& pc, int preproc_ncnl_count, c_token_t in_preproc)
{
   const chunk_tag_t *punc;
   int ch, ch1;

   /* Save off the current column */
   pc.orig_line = ctx.c.row;
   pc.orig_col  = ctx.c.col;
   pc.type      = CT_NONE;
   pc.flags     = 0;

   /**
    * Parse whitespace
    */
   if (parse_whitespace(ctx, pc))
   {
      return(true);
   }

   /**
    * Handle unknown/unhandled preprocessors
    */
   if ((in_preproc > CT_PP_BODYCHUNK) &&
       (in_preproc <= CT_PP_OTHER))
   {
      pc.str.clear();
      tok_info ss;
      ctx.save(ss);
      /* Chunk to a newline or comment */
      pc.type = CT_PREPROC_BODY;
      int last = 0;
      while (ctx.more())
      {
         int ch = ctx.peek();

         if ((ch == '\n') || (ch == '\r'))
         {
            /* Back off if this is an escaped newline */
            if (last == '\\')
            {
               ctx.restore(ss);
               pc.str.resize(pc.str.length() - 1);
            }
            break;
         }

         /* Quit on a C++ comment start */
         if ((ch == '/') && (ctx.peek(1) == '/'))
         {
            break;
         }
         last = ch;
         ctx.save(ss);

         pc.str.append(1, ctx.get());
      }
      if (pc.str.size() > 0)
      {
         return(true);
      }
   }

   /**
    * Detect backslash-newline
    */
   if ((ctx.peek() == '\\') && parse_bs_newline(ctx, pc))
   {
      return(true);
   }

   /**
    * Parse comments
    */
   if (parse_comment(fpd, ctx, pc))
   {
      return(true);
   }

   /* Check for C# literal strings, ie @"hello" and identifiers @for*/
   if (((fpd.lang_flags & LANG_CS) != 0) && (ctx.peek() == '@'))
   {
      if (ctx.peek(1) == '"')
      {
         parse_cs_string(ctx, pc);
         return(true);
      }
      /* check for non-keyword identifiers such as @if @switch, etc */
      if (CharTable::IsKw1(ctx.peek(1)))
      {
         parse_word(fpd, ctx, pc, true, preproc_ncnl_count, in_preproc);
         return(true);
      }
   }

   /* handle C++0x strings u8"x" u"x" U"x" R"x" u8R"XXX(I'm a "raw UTF-8" string.)XXX" */
   ch = ctx.peek();
   if (((fpd.lang_flags & LANG_CPP) != 0) &&
       ((ch == 'u') || (ch == 'U') || (ch == 'R')))
   {
      int idx = 0;
      bool is_real = false;

      if ((ch == 'u') && (ctx.peek(1) == '8'))
      {
         idx = 2;
      }
      else if (tolower(ch) == 'u')
      {
         idx++;
      }

      if (ctx.peek(idx) == 'R')
      {
         idx++;
         is_real = true;
      }
      if (ctx.peek(idx) == '"')
      {
         if (is_real)
         {
            if (parse_cr_string(ctx, pc, idx))
            {
               return(true);
            }
         }
         else
         {
            if (parse_string(ctx, pc, idx, true))
            {
               parse_suffix(ctx, pc, true);
               return(true);
            }
         }
      }
   }

   /* PAWN specific stuff */
   if ((fpd.lang_flags & LANG_PAWN) != 0)
   {
      /* Check for PAWN strings: \"hi" or !"hi" or !\"hi" or \!"hi" */
      if ((ctx.peek() == '\\') || (ctx.peek() == '!'))
      {
         if (ctx.peek(1) == '"')
         {
            parse_string(ctx, pc, 1, (ctx.peek() == '!'));
            return(true);
         }
         else if (((ctx.peek(1) == '\\') || (ctx.peek(1) == '!')) &&
                  (ctx.peek(2) == '"'))
         {
            parse_string(ctx, pc, 2, false);
            return(true);
         }
      }
   }

   /**
    * Parse strings and character constants
    */

   if (parse_number(ctx, pc))
   {
      return(true);
   }

   if ((fpd.lang_flags & LANG_D) != 0)
   {
      /* D specific stuff */
      if (d_parse_string(ctx, pc))
      {
         return(true);
      }
   }
   else
   {
      /* Not D stuff */

      /* Check for L'a', L"abc", 'a', "abc", <abc> strings */
      ch  = ctx.peek();
      ch1 = ctx.peek(1);
      if ((((ch == 'L') || (ch == 'S')) &&
           ((ch1 == '"') || (ch1 == '\''))) ||
          (ch == '"') ||
          (ch == '\'') ||
          ((ch == '<') && (in_preproc == CT_PP_INCLUDE)))
      {
         parse_string(ctx, pc, isalpha(ch) ? 1 : 0, true);
         return(true);
      }

      if ((ch == '<') && (in_preproc == CT_PP_DEFINE))
      {
         if (chunk_get_tail(fpd)->type == CT_MACRO)
         {
            /* We have "#define XXX <", assume '<' starts an include string */
            parse_string(ctx, pc, 0, false);
            return(true);
         }
      }
   }

   /* Check for Objective C literals */
   if ((fpd.lang_flags & LANG_OC) && (ctx.peek() == '@'))
   {
      int nc = ctx.peek(1);
      if ((nc == '"') || (nc == '\''))
      {
         /* literal string */
         parse_string(ctx, pc, 1, true);
         return true;
      }
      else if ((nc >= '0') && (nc <= '9'))
      {
         /* literal number */
         pc.str.append(1, ctx.get());  /* store the '@' */
         parse_number(ctx, pc);
         return true;
      }
   }

   /* Check for pawn/ObjectiveC/Java and normal identifiers */
   if (CharTable::IsKw1(ctx.peek()) ||
       ((ctx.peek() == '@') && CharTable::IsKw1(ctx.peek(1))))
   {
      parse_word(fpd, ctx, pc, false, preproc_ncnl_count, in_preproc);
      return(true);
   }

   /* see if we have a punctuator */
   char punc_txt[4];
   punc_txt[0] = ctx.peek();
   punc_txt[1] = ctx.peek(1);
   punc_txt[2] = ctx.peek(2);
   punc_txt[3] = ctx.peek(3);
   if ((punc = find_punctuator(punc_txt, fpd.lang_flags)) != NULL)
   {
      int cnt = strlen(punc->tag);
      while (cnt--)
      {
         pc.str.append(1, ctx.get());
      }
      pc.type   = punc->type;
      pc.flags |= PCF_PUNCTUATOR;
      return(true);
   }

   /* throw away this character */
   pc.type = CT_UNKNOWN;
   pc.str.append(1, ctx.get());

   LOG_FMT(LWARN, "%s:%d Garbage in col %d: %x\n",
           fpd.filename, pc.orig_line, (int)ctx.c.col, pc.str[0]);
   return(true);
}


/**
 * This function parses or tokenizes the whole buffer into a list.
 * It has to do some tricks to parse preprocessors.
 *
 *  - trailing whitespace are removed.
 *  - leading space & tabs are converted to the appropriate format.
 *
 */
void tokenize(fp_data& fpd)
{
   tok_ctx            ctx(fpd.data);
   chunk_t            chunk;
   chunk_t            *pc    = NULL;
   chunk_t            *rprev = NULL;
   struct parse_frame frm;
   int preproc_ncnl_count = 0;
   c_token_t in_preproc = CT_NONE;

   memset(&frm, 0, sizeof(frm));

   while (ctx.more())
   {
      chunk.reset();
      if (!parse_next(fpd, ctx, chunk, preproc_ncnl_count, in_preproc))
      {
         LOG_FMT(LWARN, "%s:%d Bailed before the end?\n",
                 fpd.filename, ctx.c.row);
         break;
      }

      /* Don't create an entry for whitespace or comments */
      if (chunk.type == CT_WHITESPACE)
      {
         continue;
      }

      if (chunk.type == CT_NL_CONT)
      {
         chunk.str       = "\\\n";
      }

      /* Strip trailing whitespace (for CPP comments and PP blocks) */
      while ((chunk.str.size() > 0) &&
             ((chunk.str[chunk.str.size() - 1] == ' ') ||
              (chunk.str[chunk.str.size() - 1] == '\t')))
      {
         chunk.str.resize(chunk.str.length() - 1);
      }

      /* Store off the end column */
      chunk.orig_col_end = ctx.c.col;

      /* Add the chunk to the list */
      rprev = pc;
      if (rprev != NULL)
      {
         pc->flags |= rprev->flags & PCF_COPY_FLAGS;

         /* a newline can't be in a preprocessor */
         if (pc->type == CT_NEWLINE)
         {
            pc->flags &= ~PCF_IN_PREPROC;
         }
      }
      pc = chunk_add_before(fpd, &chunk, NULL);

      /* A newline marks the end of a preprocessor */
      if (pc->type == CT_NEWLINE)
      {
         in_preproc = CT_NONE;
         preproc_ncnl_count = 0;
      }

      /* Special handling for preprocessor stuff */
      if (in_preproc != CT_NONE)
      {
         pc->flags |= PCF_IN_PREPROC;

         /* Count words after the preprocessor */
         if (!chunk_is_newline(pc))
         {
            preproc_ncnl_count++;
         }

         /* Figure out the type of preprocessor for #include parsing */
         if (in_preproc == CT_PREPROC)
         {
            if ((pc->type < CT_PP_DEFINE) || (pc->type > CT_PP_OTHER))
            {
               pc->type = CT_PP_OTHER;
            }
            in_preproc = pc->type;
         }
      }
      else
      {
         /* Check for a preprocessor start */
         if ((pc->type == CT_POUND) &&
             ((rprev == NULL) || (rprev->type == CT_NEWLINE)))
         {
            pc->type       = CT_PREPROC;
            pc->flags     |= PCF_IN_PREPROC;
            in_preproc = CT_PREPROC;
         }
      }
   }
}


// /**
//  * A simplistic fixed-sized needle in the fixed-size haystack string search.
//  */
// int str_find(const char *needle, int needle_len,
//              const char *haystack, int haystack_len)
// {
//    int idx;
//
//    for (idx = 0; idx < (haystack_len - needle_len); idx++)
//    {
//       if (memcmp(needle, haystack + idx, needle_len) == 0)
//       {
//          return(idx);
//       }
//    }
//    return(-1);
// }
