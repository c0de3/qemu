// Use ERRP_AUTO_PROPAGATE (see include/qapi/error.h)
//
// Copyright (c) 2020 Virtuozzo International GmbH.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see
// <http://www.gnu.org/licenses/>.
//
// Usage example:
// spatch --sp-file scripts/coccinelle/auto-propagated-errp.cocci \
//  --macro-file scripts/cocci-macro-file.h --in-place \
//  --no-show-diff --max-width 80 FILES...
//
// Note: --max-width 80 is needed because coccinelle default is less
// than 80, and without this parameter coccinelle may reindent some
// lines which fit into 80 characters but not to coccinelle default,
// which in turn produces extra patch hunks for no reason.

// Switch unusual Error ** parameter names to errp
// (this is necessary to use ERRP_AUTO_PROPAGATE).
//
// Disable optional_qualifier to skip functions with
// "Error *const *errp" parameter.
//
// Skip functions with "assert(_errp && *_errp)" statement, because
// that signals unusual semantics, and the parameter name may well
// serve a purpose. (like nbd_iter_channel_error()).
//
// Skip util/error.c to not touch, for example, error_propagate() and
// error_propagate_prepend().
@ depends on !(file in "util/error.c") disable optional_qualifier@
identifier fn;
identifier _errp != errp;
@@

 fn(...,
-   Error **_errp
+   Error **errp
    ,...)
 {
(
     ... when != assert(_errp && *_errp)
&
     <...
-    _errp
+    errp
     ...>
)
 }

// Add invocation of ERRP_AUTO_PROPAGATE to errp-functions where
// necessary
//
// Note, that without "when any" the final "..." does not mach
// something matched by previous pattern, i.e. the rule will not match
// double error_prepend in control flow like in
// vfio_set_irq_signaling().
//
// Note, "exists" says that we want apply rule even if it matches not
// on all possible control flows (otherwise, it will not match
// standard pattern when error_propagate() call is in if branch).
@ disable optional_qualifier exists@
identifier fn, local_err;
symbol errp;
@@

 fn(..., Error **errp, ...)
 {
+   ERRP_AUTO_PROPAGATE();
    ...  when != ERRP_AUTO_PROPAGATE();
(
(
    error_append_hint(errp, ...);
|
    error_prepend(errp, ...);
|
    error_vprepend(errp, ...);
)
    ... when any
|
    Error *local_err = NULL;
    ...
(
    error_propagate_prepend(errp, local_err, ...);
|
    error_propagate(errp, local_err);
)
    ...
)
 }


// Match functions with propagation of local error to errp.
// We want to refer these functions in several following rules, but I
// don't know a proper way to inherit a function, not just its name
// (to not match another functions with same name in following rules).
// Not-proper way is as follows: rename errp parameter in functions
// header and match it in following rules. Rename it back after all
// transformations.
//
// The simplest case of propagation scheme is single definition of
// local_err with at most one error_propagate_prepend or
// error_propagate on each control-flow. Still, we want to match more
// complex schemes too. We'll warn them with help of further rules.
@rule1 disable optional_qualifier exists@
identifier fn, local_err;
symbol errp;
@@

 fn(..., Error **
-    errp
+    ____
    , ...)
 {
     ...
     Error *local_err = NULL;
     ...
(
     error_propagate_prepend(errp, local_err, ...);
|
     error_propagate(errp, local_err);
)
     ...
 }


// Warn several Error * definitions.
@check1 disable optional_qualifier exists@
identifier fn = rule1.fn, local_err, local_err2;
@@

 fn(..., Error ** ____, ...)
 {
     ...
     Error *local_err = NULL;
     ... when any
     Error *local_err2 = NULL;
     ... when any
 }

@ script:python @
fn << check1.fn;
@@

print('Warning: function {} has several definitions of '
      'Error * local variable'.format(fn))

// Warn several propagations in control flow.
@check2 disable optional_qualifier exists@
identifier fn = rule1.fn;
symbol errp;
position p1, p2;
@@

 fn(..., Error ** ____, ...)
 {
     ...
(
     error_propagate_prepend(errp, ...);@p1
|
     error_propagate(errp, ...);@p1
)
     ...
(
     error_propagate_prepend(errp, ...);@p2
|
     error_propagate(errp, ...);@p2
)
     ... when any
 }

@ script:python @
fn << check2.fn;
p1 << check2.p1;
p2 << check2.p2;
@@

print('Warning: function {} propagates to errp several times in '
      'one control flow: at {}:{} and then at {}:{}'.format(
          fn, p1[0].file, p1[0].line, p2[0].file, p2[0].line))

// Convert special case with goto separately.
// I tried merging this into the following rule the obvious way, but
// it made Coccinelle hang on block.c
//
// Note interesting thing: if we don't do it here, and try to fixup
// "out: }" things later after all transformations (the rule will be
// the same, just without error_propagate() call), coccinelle fails to
// match this "out: }".
@ disable optional_qualifier@
identifier rule1.fn, rule1.local_err, out;
symbol errp;
@@

 fn(..., Error ** ____, ...)
 {
     <...
-    goto out;
+    return;
     ...>
- out:
-    error_propagate(errp, local_err);
 }

// Convert most of local_err related stuff.
//
// Note, that we update everything related to matched by rule1
// function name and local_err name. We may match something not
// related to the pattern matched by rule1. For example, local_err may
// be defined with the same name in different blocks inside one
// function, and in one block follow the propagation pattern and in
// other block doesn't. Or we may have several functions with the same
// name (for different configurations).
//
// Note also that errp-cleaning functions
//   error_free_errp
//   error_report_errp
//   error_reportf_errp
//   warn_report_errp
//   warn_reportf_errp
// are not yet implemented. They must call corresponding Error* -
// freeing function and then set *errp to NULL, to avoid further
// propagation to original errp (consider ERRP_AUTO_PROPAGATE in use).
// For example, error_free_errp may look like this:
//
//    void error_free_errp(Error **errp)
//    {
//        error_free(*errp);
//        *errp = NULL;
//    }
@ disable optional_qualifier exists@
identifier rule1.fn, rule1.local_err;
expression list args;
symbol errp;
@@

 fn(..., Error ** ____, ...)
 {
     <...
(
-    Error *local_err = NULL;
|

// Convert error clearing functions
(
-    error_free(local_err);
+    error_free_errp(errp);
|
-    error_report_err(local_err);
+    error_report_errp(errp);
|
-    error_reportf_err(local_err, args);
+    error_reportf_errp(errp, args);
|
-    warn_report_err(local_err);
+    warn_report_errp(errp);
|
-    warn_reportf_err(local_err, args);
+    warn_reportf_errp(errp, args);
)
?-    local_err = NULL;

|
-    error_propagate_prepend(errp, local_err, args);
+    error_prepend(errp, args);
|
-    error_propagate(errp, local_err);
|
-    &local_err
+    errp
)
     ...>
 }

// Convert remaining local_err usage. For example, different kinds of
// error checking in if conditionals. We can't merge this into
// previous hunk, as this conflicts with other substitutions in it (at
// least with "- local_err = NULL").
@ disable optional_qualifier@
identifier rule1.fn, rule1.local_err;
symbol errp;
@@

 fn(..., Error ** ____, ...)
 {
     <...
-    local_err
+    *errp
     ...>
 }

// Always use the same pattern for checking error
@ disable optional_qualifier@
identifier rule1.fn;
symbol errp;
@@

 fn(..., Error ** ____, ...)
 {
     <...
-    *errp != NULL
+    *errp
     ...>
 }

// Revert temporary ___ identifier.
@ disable optional_qualifier@
identifier rule1.fn;
@@

 fn(..., Error **
-   ____
+   errp
    , ...)
 {
     ...
 }
