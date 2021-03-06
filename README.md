toks
====

toks is a developer's tool for analyzing source code. Much like a normal compiler it performs tokenization and lexical analysis to identify and categorize the most useful tokens in the source code, such as:

 * Function definitions/declarations/references
 * Preprocessor definitions/macros
 * Type definitions
 * struct/union/enum definitions
 * enum value definitions
 * Variable definitions/declarations

However, unlike a normal compiler it uses fuzzy parsing and heuristics to avoid the need for setting up compiler options and include paths (for languages that use such features), thus making it easy and quick to use on an unknown code base with a complicated build system. The result of the analysis is stored in an index for quick retrieval.

Currently, the following languages are supported:

 * C
 * C++
 * Objective-C
 * Java
 * ECMA (JavaScript/ActionScript/JScript)
 * C#
 * D
 * Pawn
 * Vala

Installation
------------

Prebuilt deb and rpm packages are available for 64bit Ubuntu/Fedora (and compatible derivatives) as well as a zipped executable for Windows (works on both 32bit and 64bit versions). The deb and rpm packages will place the executable in /usr/bin. On Windows, the executable should be placed in a directory that is included in the system path.

The latest release is always available at https://github.com/thomasthorsen/toks/releases.

Usage
-----

To obtain general usage information, simply execute toks with no parameters.

Building the index (uses default name TOKS for index file):

    > toks source1.c source2.c source3.c ... sourceN.c

The analysis of a particular source file will only be performed if the contents of the file has changed relative to the last time the file was analysed. The indexing can be rerun at any time with the same set of source files or a subset or additional/new files to incrementally update the index. Source files that no longer exists in the file system will automatically be removed from the index when doing an index update.

Looking up an identifer:

    > toks --id my_identifier

To restrict the search to only references/definitions/declarations use the --refs/--defs/--decls options respectively.

It is possible to use ? and * as wildcards. The asterisk sign represents zero or multiple numbers or characters. The ? represents a single number or character. Example for finding all definitions starting with "my_":

    > toks --defs --id my_*

Example output:

    > toks --id print_event_filter
    kernel/trace/trace.h:1017:13 <global> FUNCTION DECL print_event_filter
    kernel/trace/trace_events_filter.c:649:6 <global> FUNCTION DEF print_event_filter
    kernel/trace/trace_events.c:1004:17 event_filter_read{} FUNCTION REF print_event_filter

The first part shows the location in the form filename:line:column followed by the scope and type of identifier, in this case a function with global scope. There are three entries for this particular identifier, one declaration, one definition and a reference inside the function body of event_filter_read (indicated by the curly brackets in the scope specification).

Building from source
--------------------

The build system is CMake, with a simple make wrapper. At the top level, just type "make" and it will invoke CMake automatically and launch the generated build system and build a native executable for the host machine. This probably only works on Linux or unix-like systems or with mingw64 on Windows. The resulting executable is in builds/native.

It is also possible to cross-compile for Windows on a Linux or unix-like system. Just type "make win64". You need to have the mingw64 cross compilers installed, and you may need to adjust the name of the compiler in scripts/toolchain-x86_64-mingw32.cmake depending on how it is named on the particular host system (the provided toolchain file works with the Ubuntu naming). 

Finally, it is possible to build to build using Docker, so you don't have to worry about dependencies, execute "dockerbuild.sh" which will create an Ubuntu based Docker image and launch a container that builds the code for Ubuntu. To cross-compile for Windows, execute "dockerbuild.sh win64".

History
-------

This project is based on the tokenizer/lexer in uncrustify by Ben Gardner as that code base already implemented most of what was required to identify the tokens of interest to make this project possible. This project started out as a clone of that project which is evident throughout the code base. A big thank you to Ben Gardner for releasing the source code under GPL.
