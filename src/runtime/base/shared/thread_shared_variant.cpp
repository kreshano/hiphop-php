/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010 Facebook, Inc. (http://www.facebook.com)          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <runtime/base/shared/thread_shared_variant.h>
#include <runtime/ext/ext_variable.h>
#include <runtime/base/shared/shared_map.h>
#include <runtime/base/runtime_option.h>

using namespace std;

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

ThreadSharedVariant::ThreadSharedVariant(CVarRef source, bool serialized,
                                         bool inner /* = false */)
  : m_owner(true) {
  ASSERT(!serialized || source.isString());

  m_ref = 1;

  switch (source.getType()) {
  case KindOfBoolean:
    {
      m_type = KindOfBoolean;
      m_data.num = source.toBoolean();
      break;
    }
  case KindOfByte:
  case KindOfInt16:
  case KindOfInt32:
  case KindOfInt64:
    {
      m_type = KindOfInt64;
      m_data.num = source.toInt64();
      break;
    }
  case KindOfDouble:
    {
      m_type = KindOfDouble;
      m_data.dbl = source.toDouble();
      break;
    }
  case LiteralString:
  case KindOfStaticString:
  case KindOfString:
    {
      String s = source.toString();
      m_type = serialized ? KindOfObject : KindOfString;
      m_data.str = s->copy(true);
      break;
    }
  case KindOfArray:
    {
      m_type = KindOfArray;

      ArrayData *arr = source.getArrayData();

      if (!inner) {
        // only need to call hasInternalReference() on the toplevel array
        PointerSet seen;
        if (arr->hasInternalReference(seen)) {
          m_serializedArray = true;
          m_shouldCache = true;
          String s = f_serialize(source);
          m_data.str = new StringData(s.data(), s.size(), CopyString);
          break;
        }
      }

      size_t size = arr->size();
      m_data.map = new MapData(size);

      uint i = 0;
      for (ArrayIter it(arr); !it.end(); it.next(), i++) {
        ThreadSharedVariant* key = createAnother(it.first(), false);
        ThreadSharedVariant* val = createAnother(it.second(), false, true);
        if (val->shouldCache()) m_shouldCache = true;
        m_data.map->set(i, key, val);
      }
      break;
    }
  default:
    {
      m_type = KindOfObject;
      m_shouldCache = true;
      String s = f_serialize(source);
      m_data.str = new StringData(s.data(), s.size(), CopyString);
      break;
    }
  }
}

Variant ThreadSharedVariant::toLocal() {
  ASSERT(m_owner);
  switch (m_type) {
  case KindOfBoolean:
    {
      return (bool)m_data.num;
    }
  case KindOfInt64:
    {
      return m_data.num;
    }
  case KindOfDouble:
    {
      return m_data.dbl;
    }
  case KindOfString:
    {
      if (m_data.str->isStatic()) return m_data.str;
      return NEW(StringData)(this);
    }
  case KindOfArray:
    {
      if (m_serializedArray) {
        return f_unserialize(String(m_data.str->data(), m_data.str->size(),
                                    AttachLiteral));
      }
      return NEW(SharedMap)(this);
    }
  default:
    {
      ASSERT(m_type == KindOfObject);
      return f_unserialize(String(m_data.str->data(), m_data.str->size(),
                                  AttachLiteral));
    }
  }
}

void ThreadSharedVariant::dump(std::string &out) {
  out += "ref(";
  out += boost::lexical_cast<string>(m_ref);
  out += ") ";
  switch (m_type) {
  case KindOfBoolean:
    out += "boolean: ";
    out += m_data.num ? "true" : "false";
    break;
  case KindOfInt64:
    out += "int: ";
    out += boost::lexical_cast<string>(m_data.num);
    break;
  case KindOfDouble:
    out += "double: ";
    out += boost::lexical_cast<string>(m_data.dbl);
    break;
  case KindOfString:
    out += "string(";
    out += boost::lexical_cast<string>(stringLength());
    out += "): ";
    out += stringData();
    break;
  case KindOfArray:
    if (m_serializedArray) {
      out += "array: ";
      out += m_data.str->data();
    } else {
      SharedMap(this).dump(out);
    }
    break;
  default:
    out += "object: ";
    out += m_data.str->data();
    break;
  }
  out += "\n";
}

ThreadSharedVariant::~ThreadSharedVariant() {
  switch (m_type) {
  case KindOfString:
  case KindOfObject:
    if (m_owner) {
      m_data.str->destruct();
    }
    break;
  case KindOfArray:
    {
      if (m_serializedArray) {
        if (m_owner) {
          m_data.str->destruct();
        }
        break;
      }

      ASSERT(m_owner);
      delete m_data.map;
    }
    break;
  default:
    break;
  }
}

///////////////////////////////////////////////////////////////////////////////

const char *ThreadSharedVariant::stringData() const {
  ASSERT(is(KindOfString));
  return m_data.str->data();
}

size_t ThreadSharedVariant::stringLength() const {
  ASSERT(is(KindOfString));
  return m_data.str->size();
}

size_t ThreadSharedVariant::arrSize() const {
  ASSERT(is(KindOfArray));
  return m_data.map->size;
}

int ThreadSharedVariant::getIndex(CVarRef key) {
  ASSERT(is(KindOfArray));
  switch (key.getType()) {
  case KindOfByte:
  case KindOfInt16:
  case KindOfInt32:
  case KindOfInt64: {
    int64 num = key.getNumData();
    Int64ToIntMap::const_iterator it = m_data.map->intMap.find(num);
    if (it == m_data.map->intMap.end()) return -1;
    return it->second;
  }
  case KindOfStaticString:
  case KindOfString: {
    StringData *sd = key.getStringData();
    StringDataToIntMap::const_iterator it = m_data.map->strMap.find(sd);
    if (it == m_data.map->strMap.end()) return -1;
    return it->second;
  }
  case LiteralString: {
    StringData sd(key.getLiteralString(), AttachLiteral);
    StringDataToIntMap::const_iterator it = m_data.map->strMap.find(&sd);
    if (it == m_data.map->strMap.end()) return -1;
    return it->second;
  }
  default:
    // No other types are legitimate keys
    break;
  }
  return -1;
}

SharedVariant* ThreadSharedVariant::get(CVarRef key) {
  int idx = getIndex(key);
  if (idx != -1) {
    return m_data.map->vals[idx];
  }
  return NULL;
}

bool ThreadSharedVariant::exists(CVarRef key) {
  ASSERT(is(KindOfArray));
  int idx = getIndex(key);
  return idx != -1;
}

void ThreadSharedVariant::loadElems(ArrayData *&elems,
                                    const SharedMap &sharedMap,
                                    bool keepRef /* = false */) {
  ASSERT(is(KindOfArray));
  uint count = arrSize();
  ArrayInit ai(count, false, keepRef);
  for (uint i = 0; i < count; i++) {
    ai.set(i, m_data.map->keys[i]->toLocal(), sharedMap.getValue(i), -1, true);
  }
  elems = ai.create();
  if (elems->isStatic()) elems = elems->copy();
}

ThreadSharedVariant *ThreadSharedVariant::createAnother
(CVarRef source, bool serialized, bool inner /* = false */) {
  return new ThreadSharedVariant(source, serialized, inner);
}

ThreadSharedVariant *ThreadSharedVariantLockedRefs::createAnother
(CVarRef source, bool serialized, bool inner /* = false */) {
  return new ThreadSharedVariantLockedRefs(source, serialized, m_lock, inner);
}

///////////////////////////////////////////////////////////////////////////////
}
