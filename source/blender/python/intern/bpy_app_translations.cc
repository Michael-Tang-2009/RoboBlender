/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file defines a singleton py object accessed via 'bpy.app.translations',
 * which exposes various data and functions useful in i18n work.
 * Most notably, it allows to extend main translations with Python dictionaries.
 */

#include <Python.h>
/* XXX Why bloody hell isn't that included in Python.h???? */
#include <structmember.h>

#include "BLI_utildefines.h"

#include "BPY_extern.hh"
#include "bpy_app_translations.hh"

#include "MEM_guardedalloc.h"

#include "BLT_lang.hh"
#include "BLT_translation.hh"

#include "RNA_types.hh"

#include "../generic/python_utildefines.hh"

#ifdef WITH_INTERNATIONAL
#  include "BLI_ghash.h"
#  include "BLI_string.h"
#endif

/* ------------------------------------------------------------------- */
/** \name Local Struct to Store Translation
 * \{ */

struct BlenderAppTranslations {
  PyObject_HEAD
  /** The string used to separate context from actual message in PY_TRANSLATE RNA props. */
  const char *context_separator;
  /** A "named tuple" (StructSequence actually...) containing all C-defined contexts. */
  PyObject *contexts;
  /** A readonly mapping {C context id: python id}  (actually, a MappingProxy). */
  PyObject *contexts_C_to_py;
  /**
   * A Python dictionary containing all registered Python dictionaries
   * (order is more or less random, first match wins!).
   */
  PyObject *py_messages;
};

/* Our singleton instance pointer */
static BlenderAppTranslations *_translations = nullptr;

/** \} */

/* ------------------------------------------------------------------- */
/** \name Helpers for GHash
 * \{ */

#ifdef WITH_INTERNATIONAL

struct GHashKey {
  const char *msgctxt;
  const char *msgid;
};

static GHashKey *_ghashutil_keyalloc(const void *msgctxt, const void *msgid)
{
  GHashKey *key = static_cast<GHashKey *>(MEM_mallocN(sizeof(GHashKey), "Py i18n GHashKey"));
  key->msgctxt = BLI_strdup(static_cast<const char *>(
      BLT_is_default_context(static_cast<const char *>(msgctxt)) ? BLT_I18NCONTEXT_DEFAULT_BPYRNA :
                                                                   msgctxt));
  key->msgid = BLI_strdup(static_cast<const char *>(msgid));
  return key;
}

static uint _ghashutil_keyhash(const void *ptr)
{
  const GHashKey *key = static_cast<const GHashKey *>(ptr);
  const uint hash = BLI_ghashutil_strhash(key->msgctxt);
  return hash ^ BLI_ghashutil_strhash(key->msgid);
}

static bool _ghashutil_keycmp(const void *a, const void *b)
{
  const GHashKey *A = static_cast<const GHashKey *>(a);
  const GHashKey *B = static_cast<const GHashKey *>(b);

  /* NOTE: comparing msgid first, most of the time it will be enough! */
  if (BLI_ghashutil_strcmp(A->msgid, B->msgid) == false) {
    return BLI_ghashutil_strcmp(A->msgctxt, B->msgctxt);
  }
  return true; /* true means they are not equal! */
}

static void _ghashutil_keyfree(void *ptr)
{
  const GHashKey *key = static_cast<const GHashKey *>(ptr);

  /* We assume both msgctxt and msgid were BLI_strdup'ed! */
  MEM_freeN((void *)key->msgctxt);
  MEM_freeN((void *)key->msgid);
  MEM_freeN((void *)key);
}

#  define _ghashutil_valfree MEM_freeN

/** \} */

/* ------------------------------------------------------------------- */
/** \name Python'S Messages Cache
 * \{ */

/**
 * We cache all messages available for a given locale
 * from all Python dictionaries into a single #GHash.
 * Changing of locale is not so common, while looking for a message translation is,
 * so let's try to optimize the later as much as we can!
 * Note changing of locale, as well as (un)registering a message dict, invalidate that cache.
 */
static GHash *_translations_cache = nullptr;

static void _clear_translations_cache()
{
  if (_translations_cache) {
    BLI_ghash_free(_translations_cache, _ghashutil_keyfree, _ghashutil_valfree);
  }
  _translations_cache = nullptr;
}

static void _build_translations_cache(PyObject *py_messages, const char *locale)
{
  PyObject *uuid, *uuid_dict;
  Py_ssize_t pos = 0;
  char *language = nullptr, *language_country = nullptr, *language_variant = nullptr;

  /* For each py dict, we'll search for full locale, then language+country, then language+variant,
   * then only language keys... */
  BLT_lang_locale_explode(
      locale, &language, nullptr, nullptr, &language_country, &language_variant);

  /* Clear the cached ghash if needed, and create a new one. */
  _clear_translations_cache();
  _translations_cache = BLI_ghash_new(_ghashutil_keyhash, _ghashutil_keycmp, __func__);

  /* Iterate over all Python dictionaries. */
  while (PyDict_Next(py_messages, &pos, &uuid, &uuid_dict)) {
    PyObject *lang_dict;

#  if 0
    PyObject_Print(uuid_dict, stdout, 0);
    printf("\n");
#  endif

    /* Try to get first complete locale, then language+country,
     * then language+variant, then only language. */
    lang_dict = PyDict_GetItemString(uuid_dict, locale);
    if (!lang_dict && language_country) {
      lang_dict = PyDict_GetItemString(uuid_dict, language_country);
      locale = language_country;
    }
    if (!lang_dict && language_variant) {
      lang_dict = PyDict_GetItemString(uuid_dict, language_variant);
      locale = language_variant;
    }
    if (!lang_dict && language) {
      lang_dict = PyDict_GetItemString(uuid_dict, language);
      locale = language;
    }

    if (lang_dict) {
      PyObject *pykey, *trans;
      Py_ssize_t ppos = 0;

      if (!PyDict_Check(lang_dict)) {
        printf("WARNING! In translations' dict of \"");
        PyObject_Print(uuid, stdout, Py_PRINT_RAW);
        printf("\":\n");
        printf(
            "    Each language key must have a dictionary as value, \"%s\" is not valid, "
            "skipping: ",
            locale);
        PyObject_Print(lang_dict, stdout, Py_PRINT_RAW);
        printf("\n");
        continue;
      }

      /* Iterate over all translations of the found language dict, and populate our ghash cache. */
      while (PyDict_Next(lang_dict, &ppos, &pykey, &trans)) {
        const char *msgctxt = nullptr, *msgid = nullptr;
        bool invalid_key = false;

        if ((PyTuple_CheckExact(pykey) == false) || (PyTuple_GET_SIZE(pykey) != 2)) {
          invalid_key = true;
        }
        else {
          PyObject *tmp = PyTuple_GET_ITEM(pykey, 0);
          if (tmp == Py_None) {
            msgctxt = BLT_I18NCONTEXT_DEFAULT_BPYRNA;
          }
          else if (PyUnicode_Check(tmp)) {
            msgctxt = PyUnicode_AsUTF8(tmp);
          }
          else {
            invalid_key = true;
          }

          tmp = PyTuple_GET_ITEM(pykey, 1);
          if (PyUnicode_Check(tmp)) {
            msgid = PyUnicode_AsUTF8(tmp);
          }
          else {
            invalid_key = true;
          }
        }

        if (invalid_key) {
          printf("WARNING! In translations' dict of \"");
          PyObject_Print(uuid, stdout, Py_PRINT_RAW);
          printf("\", %s language:\n", locale);
          printf(
              "    Keys must be tuples of (msgctxt [string or None], msgid [string]), "
              "this one is not valid, skipping: ");
          PyObject_Print(pykey, stdout, Py_PRINT_RAW);
          printf("\n");
          continue;
        }
        if (PyUnicode_Check(trans) == false) {
          printf("WARNING! In translations' dict of \"");
          PyObject_Print(uuid, stdout, Py_PRINT_RAW);
          printf("\":\n");
          printf("    Values must be strings, this one is not valid, skipping: ");
          PyObject_Print(trans, stdout, Py_PRINT_RAW);
          printf("\n");
          continue;
        }

        /* Do not overwrite existing keys! */
        if (BPY_app_translations_py_pgettext(msgctxt, msgid) == msgid) {
          GHashKey *key = _ghashutil_keyalloc(msgctxt, msgid);
          Py_ssize_t trans_str_len;
          const char *trans_str = PyUnicode_AsUTF8AndSize(trans, &trans_str_len);
          BLI_ghash_insert(_translations_cache, key, BLI_strdupn(trans_str, trans_str_len));
        }
      }
    }
  }

  /* Clean up! */
  MEM_SAFE_FREE(language);
  MEM_SAFE_FREE(language_country);
  MEM_SAFE_FREE(language_variant);
}

const char *BPY_app_translations_py_pgettext(const char *msgctxt, const char *msgid)
{
#  define STATIC_LOCALE_SIZE 32 /* Should be more than enough! */

  GHashKey key;
  static char locale[STATIC_LOCALE_SIZE] = "";
  const char *tmp;

  /* Just in case, should never happen! */
  if (!_translations) {
    return msgid;
  }

  tmp = BLT_lang_get();
  if (!STREQ(tmp, locale) || !_translations_cache) {
    PyGILState_STATE _py_state;

    STRNCPY(locale, tmp);

    /* Locale changed or cache does not exist, refresh the whole cache! */
    /* This func may be called from C (i.e. outside of python interpreter 'context'). */
    _py_state = PyGILState_Ensure();

    _build_translations_cache(_translations->py_messages, locale);

    PyGILState_Release(_py_state);
  }

  /* And now, simply create the key (context, messageid) and find it in the cached dict! */
  key.msgctxt = BLT_is_default_context(msgctxt) ? BLT_I18NCONTEXT_DEFAULT_BPYRNA : msgctxt;
  key.msgid = msgid;

  tmp = static_cast<const char *>(BLI_ghash_lookup(_translations_cache, &key));

  return tmp ? tmp : msgid;

#  undef STATIC_LOCALE_SIZE
}

#endif /* WITH_INTERNATIONAL */

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_py_messages_register_doc,
    ".. method:: register(module_name, translations_dict)\n"
    "\n"
    "   Registers an addon's UI translations.\n"
    "\n"
    "   .. note::\n"
    "      Does nothing when Blender is built without internationalization support.\n"
    "\n"
    "   :arg module_name: The name identifying the addon.\n"
    "   :type module_name: string\n"
    "   :arg translations_dict: A dictionary built like that:\n"
    "      ``{locale: {msg_key: msg_translation, ...}, ...}``\n"
    "   :type translations_dict: dict\n"
    "\n");
static PyObject *app_translations_py_messages_register(BlenderAppTranslations *self,
                                                       PyObject *args,
                                                       PyObject *kw)
{
#ifdef WITH_INTERNATIONAL
  static const char *kwlist[] = {"module_name", "translations_dict", nullptr};
  PyObject *module_name, *uuid_dict;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "O!O!:bpy.app.translations.register",
                                   (char **)kwlist,
                                   &PyUnicode_Type,
                                   &module_name,
                                   &PyDict_Type,
                                   &uuid_dict))
  {
    return nullptr;
  }

  if (PyDict_Contains(self->py_messages, module_name)) {
    PyErr_Format(
        PyExc_ValueError,
        "bpy.app.translations.register: translations message cache already contains some data for "
        "addon '%s'",
        (const char *)PyUnicode_AsUTF8(module_name));
    return nullptr;
  }

  PyDict_SetItem(self->py_messages, module_name, uuid_dict);

  /* Clear cached messages dict! */
  _clear_translations_cache();
#else
  (void)self;
  (void)args;
  (void)kw;
#endif

  /* And we are done! */
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_py_messages_unregister_doc,
    ".. method:: unregister(module_name)\n"
    "\n"
    "   Unregisters an addon's UI translations.\n"
    "\n"
    "   .. note::\n"
    "      Does nothing when Blender is built without internationalization support.\n"
    "\n"
    "   :arg module_name: The name identifying the addon.\n"
    "   :type module_name: string\n"
    "\n");
static PyObject *app_translations_py_messages_unregister(BlenderAppTranslations *self,
                                                         PyObject *args,
                                                         PyObject *kw)
{
#ifdef WITH_INTERNATIONAL
  static const char *kwlist[] = {"module_name", nullptr};
  PyObject *module_name;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "O!:bpy.app.translations.unregister",
                                   (char **)kwlist,
                                   &PyUnicode_Type,
                                   &module_name))
  {
    return nullptr;
  }

  if (PyDict_Contains(self->py_messages, module_name)) {
    PyDict_DelItem(self->py_messages, module_name);
    /* Clear cached messages ghash! */
    _clear_translations_cache();
  }
#else
  (void)self;
  (void)args;
  (void)kw;
#endif

  /* And we are done! */
  Py_RETURN_NONE;
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name C-defined Contexts
 * \{ */

/* This is always available (even when WITH_INTERNATIONAL is not defined). */

static PyTypeObject BlenderAppTranslationsContextsType;

static BLT_i18n_contexts_descriptor _contexts[] = BLT_I18NCONTEXTS_DESC;

/* These fields are just empty placeholders, actual values get set in app_translations_struct().
 * This allows us to avoid many handwriting, and above all,
 * to keep all context definition stuff in BLT_translation.hh! */
static PyStructSequence_Field app_translations_contexts_fields[ARRAY_SIZE(_contexts)] = {
    {nullptr}};

static PyStructSequence_Desc app_translations_contexts_desc = {
    /*name*/ "bpy.app.translations.contexts",
    /*doc*/ "This named tuple contains all predefined translation contexts",
    /*fields*/ app_translations_contexts_fields,
    /*n_in_sequence*/ ARRAY_SIZE(app_translations_contexts_fields) - 1,
};

static PyObject *app_translations_contexts_make()
{
  PyObject *translations_contexts;
  BLT_i18n_contexts_descriptor *ctxt;
  int pos = 0;

  translations_contexts = PyStructSequence_New(&BlenderAppTranslationsContextsType);
  if (translations_contexts == nullptr) {
    return nullptr;
  }

#define SetObjString(item) \
  PyStructSequence_SET_ITEM(translations_contexts, pos++, PyUnicode_FromString(item))
#define SetObjNone() \
  PyStructSequence_SET_ITEM(translations_contexts, pos++, Py_INCREF_RET(Py_None))

  for (ctxt = _contexts; ctxt->c_id; ctxt++) {
    if (ctxt->value) {
      SetObjString(ctxt->value);
    }
    else {
      SetObjNone();
    }
  }

#undef SetObjString
#undef SetObjNone

  return translations_contexts;
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Main #BlenderAppTranslations #PyObject Definition
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_contexts_doc,
    "A named tuple containing all predefined translation contexts.\n"
    "\n"
    ".. warning::\n"
    "   Never use a (new) context starting with \"" BLT_I18NCONTEXT_DEFAULT_BPYRNA
    "\", it would be internally\n"
    "   assimilated as the default one!\n");

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_contexts_C_to_py_doc,
    "A readonly dict mapping contexts' C-identifiers to their py-identifiers.");

static PyMemberDef app_translations_members[] = {
    {"contexts",
     T_OBJECT_EX,
     offsetof(BlenderAppTranslations, contexts),
     READONLY,
     app_translations_contexts_doc},
    {"contexts_C_to_py",
     T_OBJECT_EX,
     offsetof(BlenderAppTranslations, contexts_C_to_py),
     READONLY,
     app_translations_contexts_C_to_py_doc},
    {nullptr},
};

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_locale_doc,
    "The actual locale currently in use (will always return a void string when Blender "
    "is built without "
    "internationalization support).");
static PyObject *app_translations_locale_get(PyObject * /*self*/, void * /*userdata*/)
{
  return PyUnicode_FromString(BLT_lang_get());
}

/* NOTE: defining as getter, as (even if quite unlikely), this *may* change during runtime... */
PyDoc_STRVAR(
    /* Wrap. */
    app_translations_locales_doc,
    "All locales currently known by Blender (i.e. available as translations).");
static PyObject *app_translations_locales_get(PyObject * /*self*/, void * /*userdata*/)
{
  PyObject *ret;
  const EnumPropertyItem *it, *items = BLT_lang_RNA_enum_properties();
  int num_locales = 0, pos = 0;

  if (items) {
    /* This is not elegant, but simple! */
    for (it = items; it->identifier; it++) {
      if (it->value) {
        num_locales++;
      }
    }
  }

  ret = PyTuple_New(num_locales);

  if (items) {
    for (it = items; it->identifier; it++) {
      if (it->value) {
        PyTuple_SET_ITEM(ret, pos++, PyUnicode_FromString(it->description));
      }
    }
  }

  return ret;
}

static PyGetSetDef app_translations_getseters[] = {
    /* {name, getter, setter, doc, userdata} */
    {"locale", (getter)app_translations_locale_get, nullptr, app_translations_locale_doc, nullptr},
    {"locales",
     (getter)app_translations_locales_get,
     nullptr,
     app_translations_locales_doc,
     nullptr},
    {nullptr},
};

/* pgettext helper. */
static PyObject *_py_pgettext(PyObject *args,
                              PyObject *kw,
                              const char *(*_pgettext)(const char *, const char *))
{
  static const char *kwlist[] = {"msgid", "msgctxt", nullptr};

#ifdef WITH_INTERNATIONAL
  char *msgid, *msgctxt = nullptr;

  if (!PyArg_ParseTupleAndKeywords(
          args, kw, "s|z:bpy.app.translations.pgettext", (char **)kwlist, &msgid, &msgctxt))
  {
    return nullptr;
  }

  return PyUnicode_FromString((*_pgettext)(msgctxt ? msgctxt : BLT_I18NCONTEXT_DEFAULT, msgid));
#else
  PyObject *msgid, *msgctxt;
  (void)_pgettext;

  if (!PyArg_ParseTupleAndKeywords(
          args, kw, "O|O:bpy.app.translations.pgettext", (char **)kwlist, &msgid, &msgctxt))
  {
    return nullptr;
  }

  return Py_INCREF_RET(msgid);
#endif
}

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_pgettext_doc,
    ".. method:: pgettext(msgid, msgctxt=None)\n"
    "\n"
    "   Try to translate the given msgid (with optional msgctxt).\n"
    "\n"
    "   .. note::\n"
    "      The ``(msgid, msgctxt)`` parameters order has been switched compared to gettext "
    "function, to allow\n"
    "      single-parameter calls (context then defaults to BLT_I18NCONTEXT_DEFAULT).\n"
    "\n"
    "   .. note::\n"
    "      You should really rarely need to use this function in regular addon code, as all "
    "translation should be\n"
    "      handled by Blender internal code. The only exception are string containing formatting "
    "(like \"File: %r\"),\n"
    "      but you should rather use :func:`pgettext_iface`/:func:`pgettext_tip` in those cases!\n"
    "\n"
    "   .. note::\n"
    "      Does nothing when Blender is built without internationalization support (hence always "
    "returns ``msgid``).\n"
    "\n"
    "   :arg msgid: The string to translate.\n"
    "   :type msgid: string\n"
    "   :arg msgctxt: The translation context (defaults to BLT_I18NCONTEXT_DEFAULT).\n"
    "   :type msgctxt: string or None\n"
    "   :return: The translated string (or msgid if no translation was found).\n"
    "\n");
static PyObject *app_translations_pgettext(BlenderAppTranslations * /*self*/,
                                           PyObject *args,
                                           PyObject *kw)
{
  return _py_pgettext(args, kw, BLT_pgettext);
}

PyDoc_STRVAR(app_translations_pgettext_n_doc,
             ".. method:: pgettext_n(msgid, msgctxt=None)\n"
             "\n"
             "   Extract the given msgid to translation files. This is a no-op function that will "
             "only mark the string to extract, but not perform the actual translation.\n"
             "\n"
             "   .. note::\n"
             "      See :func:`pgettext` notes.\n"
             "\n"
             "   :arg msgid: The string to extract.\n"
             "   :type msgid: string\n"
             "   :arg msgctxt: The translation context (defaults to BLT_I18NCONTEXT_DEFAULT).\n"
             "   :type msgctxt: string or None\n"
             "   :return: The original string.\n"
             "\n");
static PyObject *app_translations_pgettext_n(BlenderAppTranslations * /*self*/,
                                             PyObject *args,
                                             PyObject *kw)
{
  static const char *kwlist[] = {"msgid", "msgctxt", nullptr};
  PyObject *msgid, *msgctxt;
  // (void)_pgettext;

  if (!PyArg_ParseTupleAndKeywords(
          args, kw, "O|O:bpy.app.translations.pgettext", (char **)kwlist, &msgid, &msgctxt))
  {
    return nullptr;
  }

  return Py_INCREF_RET(msgid);
}

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_pgettext_iface_doc,
    ".. method:: pgettext_iface(msgid, msgctxt=None)\n"
    "\n"
    "   Try to translate the given msgid (with optional msgctxt), if labels' translation "
    "is enabled.\n"
    "\n"
    "   .. note::\n"
    "      See :func:`pgettext` notes.\n"
    "\n"
    "   :arg msgid: The string to translate.\n"
    "   :type msgid: string\n"
    "   :arg msgctxt: The translation context (defaults to BLT_I18NCONTEXT_DEFAULT).\n"
    "   :type msgctxt: string or None\n"
    "   :return: The translated string (or msgid if no translation was found).\n"
    "\n");
static PyObject *app_translations_pgettext_iface(BlenderAppTranslations * /*self*/,
                                                 PyObject *args,
                                                 PyObject *kw)
{
  return _py_pgettext(args, kw, BLT_translate_do_iface);
}

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_pgettext_tip_doc,
    ".. method:: pgettext_tip(msgid, msgctxt=None)\n"
    "\n"
    "   Try to translate the given msgid (with optional msgctxt), if tooltips' "
    "translation is enabled.\n"
    "\n"
    "   .. note::\n"
    "      See :func:`pgettext` notes.\n"
    "\n"
    "   :arg msgid: The string to translate.\n"
    "   :type msgid: string\n"
    "   :arg msgctxt: The translation context (defaults to BLT_I18NCONTEXT_DEFAULT).\n"
    "   :type msgctxt: string or None\n"
    "   :return: The translated string (or msgid if no translation was found).\n"
    "\n");
static PyObject *app_translations_pgettext_tip(BlenderAppTranslations * /*self*/,
                                               PyObject *args,
                                               PyObject *kw)
{
  return _py_pgettext(args, kw, BLT_translate_do_tooltip);
}

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_pgettext_rpt_doc,
    ".. method:: pgettext_rpt(msgid, msgctxt=None)\n"
    "\n"
    "   Try to translate the given msgid (with optional msgctxt), if reports' translation "
    "is enabled.\n"
    "\n"
    "   .. note::\n"
    "      See :func:`pgettext` notes.\n"
    "\n"
    "   :arg msgid: The string to translate.\n"
    "   :type msgid: string\n"
    "   :arg msgctxt: The translation context (defaults to BLT_I18NCONTEXT_DEFAULT).\n"
    "   :type msgctxt: string or None\n"
    "   :return: The translated string (or msgid if no translation was found).\n"
    "\n");
static PyObject *app_translations_pgettext_rpt(BlenderAppTranslations * /*self*/,
                                               PyObject *args,
                                               PyObject *kw)
{
  return _py_pgettext(args, kw, BLT_translate_do_report);
}

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_pgettext_data_doc,
    ".. method:: pgettext_data(msgid, msgctxt=None)\n"
    "\n"
    "   Try to translate the given msgid (with optional msgctxt), if new data name's "
    "translation is enabled.\n"
    "\n"
    "   .. note::\n"
    "      See :func:`pgettext` notes.\n"
    "\n"
    "   :arg msgid: The string to translate.\n"
    "   :type msgid: string\n"
    "   :arg msgctxt: The translation context (defaults to BLT_I18NCONTEXT_DEFAULT).\n"
    "   :type msgctxt: string or None\n"
    "   :return: The translated string (or ``msgid`` if no translation was found).\n"
    "\n");
static PyObject *app_translations_pgettext_data(BlenderAppTranslations * /*self*/,
                                                PyObject *args,
                                                PyObject *kw)
{
  return _py_pgettext(args, kw, BLT_translate_do_new_dataname);
}

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_locale_explode_doc,
    ".. method:: locale_explode(locale)\n"
    "\n"
    "   Return all components and their combinations of the given ISO locale string.\n"
    "\n"
    "   >>> bpy.app.translations.locale_explode(\"sr_RS@latin\")\n"
    "   (\"sr\", \"RS\", \"latin\", \"sr_RS\", \"sr@latin\")\n"
    "\n"
    "   For non-complete locales, missing elements will be None.\n"
    "\n"
    "   :arg locale: The ISO locale string to explode.\n"
    "   :type msgid: string\n"
    "   :return: A tuple ``(language, country, variant, language_country, language@variant)``.\n"
    "\n");
static PyObject *app_translations_locale_explode(BlenderAppTranslations * /*self*/,
                                                 PyObject *args,
                                                 PyObject *kw)
{
  PyObject *ret_tuple;
  static const char *kwlist[] = {"locale", nullptr};
  const char *locale;
  char *language, *country, *variant, *language_country, *language_variant;

  if (!PyArg_ParseTupleAndKeywords(
          args, kw, "s:bpy.app.translations.locale_explode", (char **)kwlist, &locale))
  {
    return nullptr;
  }

  BLT_lang_locale_explode(
      locale, &language, &country, &variant, &language_country, &language_variant);

  ret_tuple = Py_BuildValue(
      "sssss", language, country, variant, language_country, language_variant);

  MEM_SAFE_FREE(language);
  MEM_SAFE_FREE(country);
  MEM_SAFE_FREE(variant);
  MEM_SAFE_FREE(language_country);
  MEM_SAFE_FREE(language_variant);

  return ret_tuple;
}

#if (defined(__GNUC__) && !defined(__clang__))
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

static PyMethodDef app_translations_methods[] = {
    /* Can't use METH_KEYWORDS alone, see http://bugs.python.org/issue11587 */
    {"register",
     (PyCFunction)app_translations_py_messages_register,
     METH_VARARGS | METH_KEYWORDS,
     app_translations_py_messages_register_doc},
    {"unregister",
     (PyCFunction)app_translations_py_messages_unregister,
     METH_VARARGS | METH_KEYWORDS,
     app_translations_py_messages_unregister_doc},
    {"pgettext",
     (PyCFunction)app_translations_pgettext,
     METH_VARARGS | METH_KEYWORDS | METH_STATIC,
     app_translations_pgettext_doc},
    {"pgettext_n",
     (PyCFunction)app_translations_pgettext_n,
     METH_VARARGS | METH_KEYWORDS | METH_STATIC,
     app_translations_pgettext_n_doc},
    {"pgettext_iface",
     (PyCFunction)app_translations_pgettext_iface,
     METH_VARARGS | METH_KEYWORDS | METH_STATIC,
     app_translations_pgettext_iface_doc},
    {"pgettext_tip",
     (PyCFunction)app_translations_pgettext_tip,
     METH_VARARGS | METH_KEYWORDS | METH_STATIC,
     app_translations_pgettext_tip_doc},
    {"pgettext_rpt",
     (PyCFunction)app_translations_pgettext_rpt,
     METH_VARARGS | METH_KEYWORDS | METH_STATIC,
     app_translations_pgettext_rpt_doc},
    {"pgettext_data",
     (PyCFunction)app_translations_pgettext_data,
     METH_VARARGS | METH_KEYWORDS | METH_STATIC,
     app_translations_pgettext_data_doc},
    {"locale_explode",
     (PyCFunction)app_translations_locale_explode,
     METH_VARARGS | METH_KEYWORDS | METH_STATIC,
     app_translations_locale_explode_doc},
    {nullptr},
};

#if (defined(__GNUC__) && !defined(__clang__))
#  pragma GCC diagnostic pop
#endif

static PyObject *app_translations_new(PyTypeObject *type, PyObject * /*args*/, PyObject * /*kw*/)
{
  // printf("%s (%p)\n", __func__, _translations);

  if (!_translations) {
    _translations = (BlenderAppTranslations *)type->tp_alloc(type, 0);
    if (_translations) {
      PyObject *py_ctxts;
      BLT_i18n_contexts_descriptor *ctxt;

      _translations->contexts = app_translations_contexts_make();

      py_ctxts = _PyDict_NewPresized(ARRAY_SIZE(_contexts));
      for (ctxt = _contexts; ctxt->c_id; ctxt++) {
        PyObject *val = PyUnicode_FromString(ctxt->py_id);
        PyDict_SetItemString(py_ctxts, ctxt->c_id, val);
        Py_DECREF(val);
      }
      _translations->contexts_C_to_py = PyDictProxy_New(py_ctxts);
      Py_DECREF(py_ctxts); /* The actual dict is only owned by its proxy */

      _translations->py_messages = PyDict_New();
    }
  }

  return (PyObject *)_translations;
}

static void app_translations_free(void *obj)
{
  PyObject_Del(obj);
#ifdef WITH_INTERNATIONAL
  _clear_translations_cache();
#endif
}

PyDoc_STRVAR(
    /* Wrap. */
    app_translations_doc,
    "This object contains some data/methods regarding internationalization in Blender, "
    "and allows every py script\n"
    "to feature translations for its own UI messages.\n"
    "\n");
static PyTypeObject BlenderAppTranslationsType = {
    /*ob_base*/ PyVarObject_HEAD_INIT(nullptr, 0)
    /*tp_name*/ "bpy.app._translations_type",
    /*tp_basicsize*/ sizeof(BlenderAppTranslations),
    /*tp_itemsize*/ 0,
    /*tp_dealloc*/ nullptr,
    /*tp_vectorcall_offset*/ 0,
    /*tp_getattr*/ nullptr,
    /*tp_setattr*/ nullptr,
    /*tp_as_async*/ nullptr,
    /*tp_repr*/ nullptr,
    /*tp_as_number*/ nullptr,
    /*tp_as_sequence*/ nullptr,
    /*tp_as_mapping*/ nullptr,
    /*tp_hash*/ nullptr,
    /*tp_call*/ nullptr,
    /*tp_str*/ nullptr,
    /*tp_getattro*/ nullptr,
    /*tp_setattro*/ nullptr,
    /*tp_as_buffer*/ nullptr,
    /*tp_flags*/ Py_TPFLAGS_DEFAULT,
    /*tp_doc*/ app_translations_doc,
    /*tp_traverse*/ nullptr,
    /*tp_clear*/ nullptr,
    /*tp_richcompare*/ nullptr,
    /*tp_weaklistoffset*/ 0,
    /*tp_iter*/ nullptr,
    /*tp_iternext*/ nullptr,
    /*tp_methods*/ app_translations_methods,
    /*tp_members*/ app_translations_members,
    /*tp_getset*/ app_translations_getseters,
    /*tp_base*/ nullptr,
    /*tp_dict*/ nullptr,
    /*tp_descr_get*/ nullptr,
    /*tp_descr_set*/ nullptr,
    /*tp_dictoffset*/ 0,
    /*tp_init*/ nullptr,
    /*tp_alloc*/ nullptr,
    /*tp_new*/ (newfunc)app_translations_new,
    /*tp_free*/ app_translations_free,
    /*tp_is_gc*/ nullptr,
    /*tp_bases*/ nullptr,
    /*tp_mro*/ nullptr,
    /*tp_cache*/ nullptr,
    /*tp_subclasses*/ nullptr,
    /*tp_weaklist*/ nullptr,
    /*tp_del*/ nullptr,
    /*tp_version_tag*/ 0,
    /*tp_finalize*/ nullptr,
    /*tp_vectorcall*/ nullptr,
};

PyObject *BPY_app_translations_struct()
{
  PyObject *ret;

  /* Let's finalize our contexts `PyStructSequence` definition! */
  {
    BLT_i18n_contexts_descriptor *ctxt;
    PyStructSequence_Field *desc;

    /* We really populate the contexts' fields here! */
    for (ctxt = _contexts, desc = app_translations_contexts_desc.fields; ctxt->c_id;
         ctxt++, desc++)
    {
      desc->name = ctxt->py_id;
      desc->doc = nullptr;
    }
    desc->name = desc->doc = nullptr; /* End sentinel! */

    PyStructSequence_InitType(&BlenderAppTranslationsContextsType,
                              &app_translations_contexts_desc);
  }

  if (PyType_Ready(&BlenderAppTranslationsType) < 0) {
    return nullptr;
  }

  ret = PyObject_CallObject((PyObject *)&BlenderAppTranslationsType, nullptr);

  /* prevent user from creating new instances */
  BlenderAppTranslationsType.tp_new = nullptr;
  /* Without this we can't do `set(sys.modules)` #29635. */
  BlenderAppTranslationsType.tp_hash = (hashfunc)_Py_HashPointer;

  return ret;
}

void BPY_app_translations_end()
{
/* In case the object remains in a module's name-space, see #44127. */
#ifdef WITH_INTERNATIONAL
  _clear_translations_cache();
#endif
}

/** \} */
