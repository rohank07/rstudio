/*
 * RSexp.cpp
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#define R_INTERNAL_FUNCTIONS
#include <r/RSexp.hpp>
#include <r/RInternal.hpp>

#include <algorithm>

#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/foreach.hpp>
#include <boost/numeric/conversion/cast.hpp>

#include <core/Macros.hpp>
#include <core/Log.hpp>
#include <core/DateTime.hpp>

#include <r/RExec.hpp>
#include <r/RErrorCategory.hpp>

// clean out global definitions of TRUE and FALSE so we can
// use the Rboolean variations of them
#undef TRUE
#undef FALSE

using namespace rstudio::core ;

namespace rstudio {
namespace r {
   
using namespace exec ;
   
namespace sexp {
   
std::string asString(SEXP object) 
{
    return std::string(Rf_translateChar(Rf_asChar(object)));
}
   
std::string safeAsString(SEXP object, const std::string& defValue)
{
   if (object != R_NilValue)
      return asString(object);
   else 
      return defValue;
}
    
int asInteger(SEXP object)
{
   return Rf_asInteger(object);
}
   
double asReal(SEXP object)
{
   return Rf_asReal(object);
}
   
bool asLogical(SEXP object)
{
   return Rf_asLogical(object) ? true : false;
}

bool fillVectorString(SEXP object, std::vector<std::string>* pVector)
{
   if (TYPEOF(object) != STRSXP)
      return false;
   
   int n = Rf_length(object);
   pVector->reserve(pVector->size() + n);
   for (int i = 0; i < n; i++)
      pVector->push_back(std::string(CHAR(STRING_ELT(object, i))));
   
   return true;
}

bool fillSetString(SEXP object, std::set<std::string>* pSet)
{
   if (TYPEOF(object) != STRSXP)
      return false;
   
   int n = Rf_length(object);
   for (int i = 0; i < n; i++)
      pSet->insert(std::string(CHAR(STRING_ELT(object, i))));
   
   return true;
}

std::vector<std::string> getLoadedNamespaces()
{
   std::vector<std::string> result;
   r::exec::RFunction loadedNamespaces("loadedNamespaces", "base");
   Error error = loadedNamespaces.call(&result);
   if (error)
      LOG_ERROR(error);
   return result;
}

SEXP findNamespace(const std::string& name)
{
   if (name.empty())
       return R_UnboundValue;
   
   // case 4071: namespace look up executes R code that can trip the debugger
   DisableDebugScope disableStepInto(R_GlobalEnv);

   // R_FindNamespace will throw if it fails to find a particular name.
   // Instead, we manually search the namespace registry.
   SEXP nameSEXP = Rf_install(name.c_str());
   SEXP ns = Rf_findVarInFrame(R_NamespaceRegistry, nameSEXP);
   return ns;
}
   
void listEnvironment(SEXP env, 
                     bool includeAll,
                     Protect* pProtect,
                     std::vector<Variable>* pVariables)
{
   // reset passed vars
   pVariables->clear();
   
   // get the list of environment vars (protect locally because we 
   // we don't acutally return this list to the caller
   SEXP envVarsSEXP;
   Protect rProtect(envVarsSEXP = R_lsInternal(env, includeAll ? TRUE : FALSE));

   // get variables
   std::vector<std::string> vars;
   Error error = r::sexp::extract(envVarsSEXP, &vars);
   if (error)
   {
      LOG_ERROR(error);
      return;
   }

   // populate pVariables
   BOOST_FOREACH(const std::string& var, vars)
   {
      SEXP varSEXP = R_NilValue;
      // Merely calling Rf_findVar on an active binding will fire the binding.
      // Don't try to get the SEXP for the variable in this case; leave the
      // value as nil.
      if (!isActiveBinding(var, env))
         varSEXP = Rf_findVar(Rf_install(var.c_str()), env);

      if (varSEXP != R_UnboundValue) // should never be unbound
      {
         pProtect->add(varSEXP);
         pVariables->push_back(std::make_pair(var, varSEXP));
      }
      else
      {
         LOG_WARNING_MESSAGE(
                  "Unexpected R_UnboundValue returned from R_lsInternal");
      }
   }
}

bool isActiveBinding(const std::string& name, const SEXP env)
{
   return R_BindingIsActive(Rf_install(name.c_str()), env);
}

SEXP findVar(const std::string &name, const SEXP env)
{
   return Rf_findVar(Rf_install(name.c_str()), env);
}

SEXP findVar(const std::string& name, const std::string& ns)
{
   if (name.empty())
      return R_UnboundValue;
   
   SEXP env = ns.empty() ? R_GlobalEnv : findNamespace(ns);
   
   return findVar(name, env);
}

SEXP findFunction(const std::string& name, const std::string& ns) 
{
   r::sexp::Protect protect;
   if (name.empty())
      return R_UnboundValue;
   
   SEXP env = ns.empty() ? R_GlobalEnv : findNamespace(ns);
   if (env == R_UnboundValue) return R_UnboundValue;
   
   // We might want to use `Rf_findFun`, but it calls `Rf_error`
   // on failure, which involves printing the error message out
   // to the console. To avoid this,
   // we instead attempt to find the function by manually
   // walking through the environment (and its enclosing environments)
   SEXP nameSEXP = Rf_install(name.c_str());
   
   // Search through frames until we find the global environment.
   while (env != R_EmptyEnv)
   {
      // If we're searching the global environment, then
      // try using 'Rf_findVar', as this will attempt a search
      // of R's own internal global cache.
      if (env == R_GlobalEnv)
      {
         SEXP resultSEXP = Rf_findVar(nameSEXP, R_GlobalEnv);
         if (Rf_isFunction(resultSEXP))
            return resultSEXP;
         else if (TYPEOF(resultSEXP) == PROMSXP)
         {
            protect.add(resultSEXP = Rf_eval(resultSEXP, env));
            if (Rf_isFunction(resultSEXP))
               return resultSEXP;
         }
      }
      
      // Otherwise, just perform a simple search through
      // the current frame.
      SEXP resultSEXP = Rf_findVarInFrame(env, nameSEXP);
      if (resultSEXP != R_UnboundValue)
      {
         if (Rf_isFunction(resultSEXP))
            return resultSEXP;
         else if (TYPEOF(resultSEXP) == PROMSXP)
         {
            protect.add(resultSEXP = Rf_eval(resultSEXP, env));
            if (Rf_isFunction(resultSEXP))
               return resultSEXP;
         }
      }
      
      env = ENCLOS(env);
   }
   
   return R_UnboundValue;
}   
   
std::string typeAsString(SEXP object)
{
   return Rf_type2char(TYPEOF(object));  
}

std::string classOf(SEXP objectSEXP)
{
   return asString(Rf_getAttrib(objectSEXP, Rf_install("class")));
}
   
int length(SEXP object)
{
   return Rf_length(object);
}
 
   
bool isLanguage(SEXP object)
{
   return Rf_isLanguage(object);
}
   
bool isString(SEXP object)
{
   return Rf_isString(object);
}
   
bool isMatrix(SEXP object)
{
   return Rf_isMatrix(object);
}
   
bool isDataFrame(SEXP object)
{
   return Rf_isFrame(object);
}

bool isNull(SEXP object)
{
   return Rf_isNull(object) == TRUE;
}


SEXP getNames(SEXP sexp)
{
   return Rf_getAttrib(sexp, R_NamesSymbol);
}

bool setNames(SEXP sexp, const std::vector<std::string>& names)
{
   std::size_t n = names.size();
   if (static_cast<std::size_t>(Rf_length(sexp)) != n)
      return false;

   Rf_setAttrib(sexp,
                R_NamesSymbol,
                Rf_allocVector(STRSXP, names.size()));

   SEXP namesSEXP = Rf_getAttrib(sexp, R_NamesSymbol);
   for (std::size_t i = 0; i < n; ++i)
      SET_STRING_ELT(namesSEXP, i, Rf_mkChar(names[i].c_str()));

   return true;
}
   
Error getNames(SEXP sexp, std::vector<std::string>* pNames)   
{
   // attempt to get the field names
   SEXP namesSEXP = getNames(sexp);
   
   if (namesSEXP == R_NilValue || TYPEOF(namesSEXP) != STRSXP)
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);
   else if (Rf_length(namesSEXP) != Rf_length(sexp))
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);
   
   // copy them into the vector
   for (int i=0; i<Rf_length(namesSEXP); i++)
      pNames->push_back(Rf_translateChar(STRING_ELT(namesSEXP, i)) );
   
   return Success();
}

SEXP getAttrib(SEXP object, SEXP attrib)
{
   return Rf_getAttrib(object, attrib);
}

SEXP getAttrib(SEXP object, const std::string& attrib)
{
   return getAttrib(object, Rf_install(attrib.c_str()));
}

SEXP setAttrib(SEXP object, const std::string& attrib, SEXP val)
{
   return Rf_setAttrib(object, Rf_install(attrib.c_str()), val);
}

SEXP makeWeakRef(SEXP key, SEXP val, R_CFinalizer_t fun, Rboolean onexit)
{
   return R_MakeWeakRefC(key, val, fun, onexit);
}

void registerFinalizer(SEXP s, R_CFinalizer_t fun)
{
   R_RegisterCFinalizer(s, fun);
}

SEXP makeExternalPtr(void* ptr, R_CFinalizer_t fun, Protect* pProtect)
{
   SEXP s = R_MakeExternalPtr(ptr, R_NilValue, R_NilValue);
   if (pProtect)
      pProtect->add(s);
   registerFinalizer(s, fun);
   return s;
}

void* getExternalPtrAddr(SEXP extptr)
{
   return R_ExternalPtrAddr(extptr);
}

void clearExternalPtr(SEXP extptr)
{
   R_ClearExternalPtr(extptr);
}

core::Error getNamedListSEXP(SEXP listSEXP,
                             const std::string& name,
                             SEXP* pValueSEXP)
{
   int valueIndex = indexOfElementNamed(listSEXP, name);

   if (valueIndex != -1)
   {
      // get the appropriate value
      *pValueSEXP = VECTOR_ELT(listSEXP, valueIndex);
      return core::Success();
   }
   else
   {
      // otherwise an error
      core::Error error(r::errc::ListElementNotFoundError, ERROR_LOCATION);
      error.addProperty("element", name);
      return error;
   }
}

Error extract(SEXP valueSEXP, int* pInt)
{
   if (TYPEOF(valueSEXP) != INTSXP)
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);
   
   if (Rf_length(valueSEXP) < 1)
      return Error(errc::NoDataAvailableError, ERROR_LOCATION);
      
   *pInt = INTEGER(valueSEXP)[0] ;
   return Success();
}
   
Error extract(SEXP valueSEXP, bool* pBool)
{
   if (TYPEOF(valueSEXP) != LGLSXP)
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);
   
   if (Rf_length(valueSEXP) < 1)
      return Error(errc::NoDataAvailableError, ERROR_LOCATION);
   
   *pBool = LOGICAL(valueSEXP)[0] == TRUE ? true : false ;
   return Success();
   
}

Error extract(SEXP valueSEXP, double* pDouble)
{
   if (TYPEOF(valueSEXP) != REALSXP)
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);

   if (Rf_length(valueSEXP) < 1)
      return Error(errc::NoDataAvailableError, ERROR_LOCATION);

   *pDouble = REAL(valueSEXP)[0];
   return Success();
}
   
Error extract(SEXP valueSEXP, std::vector<int>* pVector)
{
   if (TYPEOF(valueSEXP) != INTSXP)
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);
   
   pVector->clear();
   for (int i=0; i<Rf_length(valueSEXP); i++)
      pVector->push_back(INTEGER(valueSEXP)[i]);
   
   return Success(); 
}
   
Error extract(SEXP valueSEXP, std::string* pString, bool asUtf8)
{
   if (TYPEOF(valueSEXP) != STRSXP)
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);

   if (Rf_length(valueSEXP) < 1)
      return Error(errc::NoDataAvailableError, ERROR_LOCATION);

   *pString = std::string(asUtf8 ?
                  Rf_translateCharUTF8(STRING_ELT(valueSEXP, 0)) :
                  Rf_translateChar(STRING_ELT(valueSEXP, 0)));

   return Success();
}

Error extract(SEXP valueSEXP, std::vector<std::string>* pVector)
{
   if (TYPEOF(valueSEXP) != STRSXP)
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);

   pVector->clear();
   for (int i=0; i<Rf_length(valueSEXP); i++)
      pVector->push_back(Rf_translateChar(STRING_ELT(valueSEXP, i)));
   
   return Success();
}

Error extract(SEXP valueSEXP, std::set<std::string>* pSet)
{
   if (TYPEOF(valueSEXP) != STRSXP)
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);
   
   pSet->clear();
   for (int i=0; i<Rf_length(valueSEXP); i++)
      pSet->insert(Rf_translateChar(STRING_ELT(valueSEXP, i)));
   
   return Success();
}

Error extract(SEXP valueSEXP, std::map< std::string, std::set<std::string> >* pMap)
{
   if (TYPEOF(valueSEXP) != VECSXP)
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);
   
   if (Rf_length(valueSEXP) == 0)
      return Success();
   
   SEXP namesSEXP = r::sexp::getNames(valueSEXP);
   if (Rf_isNull(namesSEXP))
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);
   
   for (int i = 0; i < Rf_length(valueSEXP); ++i)
   {
      SEXP el = VECTOR_ELT(valueSEXP, i);
      std::set<std::string> contents;
      for (int j = 0; j < Rf_length(el); ++j)
         contents.insert(std::string(Rf_translateChar(STRING_ELT(el, j))));
      
      std::string name = std::string(Rf_translateChar(STRING_ELT(namesSEXP, i)));
      pMap->operator [](name) = contents;
   }
   
   return Success();
}
   
SEXP create(const json::Value& value, Protect* pProtect)
{
   // call embedded create function based on type
   if (value.type() == json::StringType)
   {
      return create(value.get_str(), pProtect);
   }
   else if (value.type() == json::IntegerType)
   {
      return create(value.get_int(), pProtect);
   }
   else if (value.type() == json::RealType)
   {
      return create(value.get_real(), pProtect);
   }
   else if (value.type() == json::BooleanType)
   {
      return create(value.get_bool(), pProtect);
   }
   else if (value.type() == json::ArrayType)
   {
      return create(value.get_array(), pProtect);
   }
   else if (value.type() == json::ObjectType)
   {
      return create(value.get_obj(), pProtect);
   }
   else if (value.is_null())
   {
      return R_NilValue;
   }
   else
   {
      return R_NilValue;
   }
}

SEXP create(const char* value, Protect* pProtect)
{
   return create(std::string(value), pProtect);
}

SEXP create(const std::string& value, Protect* pProtect)
{
   SEXP valueSEXP;
   pProtect->add(valueSEXP = Rf_allocVector(STRSXP, 1));
   SET_STRING_ELT(valueSEXP, 0, Rf_mkChar(value.c_str()));
   return valueSEXP;
}
   
SEXP create(int value, Protect* pProtect)
{
   SEXP valueSEXP;
   pProtect->add(valueSEXP = Rf_allocVector(INTSXP, 1));
   INTEGER(valueSEXP)[0] = value ;
   return valueSEXP;
}
   
SEXP create(double value, Protect* pProtect)
{
   SEXP valueSEXP;
   pProtect->add(valueSEXP = Rf_allocVector(REALSXP, 1));
   REAL(valueSEXP)[0] = value ;
   return valueSEXP;
}

SEXP create(bool value, Protect* pProtect)
{
   SEXP valueSEXP;
   pProtect->add(valueSEXP = Rf_allocVector(LGLSXP, 1));
   LOGICAL(valueSEXP)[0] = value ;
   return valueSEXP;
}

SEXP create(const json::Array& value, Protect* pProtect)
{
   // create the list
   SEXP listSEXP;
   pProtect->add(listSEXP = Rf_allocVector(VECSXP, value.size()));
   
   // add each array element to it
   for (json::Array::size_type i=0; i<value.size(); i++)
   {
      SEXP valueSEXP = create(value[i], pProtect);
      SET_VECTOR_ELT(listSEXP, i,  valueSEXP);
   }
   return listSEXP;
}
   
SEXP create(const json::Object& value, Protect* pProtect)
{
   // create the list
   SEXP listSEXP ;
   pProtect->add(listSEXP = Rf_allocVector(VECSXP, value.size()));
   
   // build list of names
   SEXP namesSEXP ;
   pProtect->add(namesSEXP = Rf_allocVector(STRSXP, value.size()));
   
   // add each object field to it
   int index = 0;
   for (json::Object::const_iterator 
            it = value.begin();
            it != value.end();
            ++it)
   {
      // set name
      SET_STRING_ELT(namesSEXP, index, Rf_mkChar(it->first.c_str()));
      
      // set value
      SEXP valueSEXP = create(it->second, pProtect);
      SET_VECTOR_ELT(listSEXP, index,  valueSEXP);
      
      // increment element index
      index++;
   }
   
   // attach names
   Rf_setAttrib(listSEXP, R_NamesSymbol, namesSEXP);
   
   // return the list
   return listSEXP;
}
   
SEXP create(const std::vector<std::string>& value, Protect* pProtect)
{
   SEXP valueSEXP;
   pProtect->add(valueSEXP = Rf_allocVector(STRSXP, value.size()));
   
   int index = 0;
   for (std::vector<std::string>::const_iterator 
        it = value.begin(); it != value.end(); ++it)
   {
      SET_STRING_ELT(valueSEXP, index++, Rf_mkChar(it->c_str()));
   }
   
   return valueSEXP;
}
   
SEXP create(const std::vector<int>& value, Protect *pProtect)
{
   SEXP valueSEXP;
   pProtect->add(valueSEXP = Rf_allocVector(INTSXP, value.size()));
   
   for (std::size_t i = 0; i < value.size(); ++i) 
      INTEGER(valueSEXP)[i] = value[i] ;
   
   return valueSEXP;
}

SEXP create(const std::vector<double>& value, Protect *pProtect)
{
   SEXP valueSEXP;
   pProtect->add(valueSEXP = Rf_allocVector(REALSXP, value.size()));
   
   for (std::size_t i = 0; i < value.size(); ++i) 
      REAL(valueSEXP)[i] = value[i] ;
   
   return valueSEXP;
}

SEXP create(const std::vector<bool>& value, Protect *pProtect)
{
   SEXP valueSEXP;
   pProtect->add(valueSEXP = Rf_allocVector(LGLSXP, value.size()));
   
   for (std::size_t i = 0; i < value.size(); ++i) 
      LOGICAL(valueSEXP)[i] = value[i] ;
   
   return valueSEXP;
}
   
namespace {  
int secondsSinceEpoch(boost::posix_time::ptime date)
{
   return boost::numeric_cast<int>(date_time::secondsSinceEpoch(date));
}}
   
SEXP create(const std::vector<boost::posix_time::ptime>& value,
            Protect* pProtect)
{
   // first create a vector of doubles containing seconds since epoch
   std::vector<int> seconds ;
   std::transform(value.begin(), 
                  value.end(),
                  std::back_inserter(seconds),
                  secondsSinceEpoch);
   
   // now turn this into an R vector and call as.POSIXct
   SEXP secondsSEXP = create(seconds, pProtect);
   SEXP posixCtSEXP = R_NilValue;           
   r::exec::RFunction asPOSIXct("as.POSIXct", secondsSEXP);
   asPOSIXct.addParam("tz", "GMT");
   asPOSIXct.addParam("origin", "1970-01-01");
   Error error = asPOSIXct.call(&posixCtSEXP, pProtect);
   if (error)
      LOG_ERROR(error);
   
   // return it
   return posixCtSEXP;
}

SEXP create(const std::map<std::string, std::vector<std::string> > &value,
            Protect *pProtect)
{
   SEXP listSEXP, namesSEXP;
   std::size_t n = value.size();
   pProtect->add(listSEXP = Rf_allocVector(VECSXP, n));
   pProtect->add(namesSEXP = Rf_allocVector(STRSXP, n));
   
   int index = 0;
   typedef std::map< std::string, std::vector<std::string> >::const_iterator iterator;
   for (iterator it = value.begin(); it != value.end(); ++it)
   {
      SET_STRING_ELT(namesSEXP, index, Rf_mkChar(it->first.c_str()));
      SET_VECTOR_ELT(listSEXP, index, r::sexp::create(it->second, pProtect));
      ++index;
   }
   
   Rf_setAttrib(listSEXP, R_NamesSymbol, namesSEXP);
   
   return listSEXP;
}
   
SEXP create(const std::vector<std::pair<std::string,std::string> >& value, 
            Protect* pProtect)
{
   // create the character vector and the names vector
   SEXP charSEXP, namesSEXP;
   pProtect->add(charSEXP = Rf_allocVector(STRSXP, value.size()));
   pProtect->add(namesSEXP = Rf_allocVector(STRSXP, value.size()));
   
   int index = 0;
   for (std::vector<std::pair<std::string,std::string> >::const_iterator 
         it = value.begin(); it != value.end(); ++it)
   {
      // set name and value
      SET_STRING_ELT(namesSEXP, index, Rf_mkChar(it->first.c_str()));
      SET_STRING_ELT(charSEXP, index,  Rf_mkChar(it->second.c_str()));
      
      // increment element index
      index++;
   }
   
   // attach names
   Rf_setAttrib(charSEXP, R_NamesSymbol, namesSEXP);
   
   // return the vector
   return charSEXP;   
}

SEXP create(const std::set<std::string> &value, Protect *pProtect)
{
   SEXP charSEXP;
   pProtect->add(charSEXP = Rf_allocVector(STRSXP, value.size()));
   
   int index = 0;
   for (std::set<std::string>::const_iterator it = value.begin();
        it != value.end();
        ++it)
   {
      SET_STRING_ELT(charSEXP, index, Rf_mkChar(it->c_str()));
      ++index;
   }
   
   return charSEXP;
}

SEXP create(const ListBuilder& builder, Protect *pProtect)
{
   int n = builder.names().size();

   SEXP resultSEXP;
   pProtect->add(resultSEXP = Rf_allocVector(VECSXP, n));

   SEXP namesSEXP;
   pProtect->add(namesSEXP = Rf_allocVector(STRSXP, n));

   for (int i = 0; i < n; i++)
   {
      SET_VECTOR_ELT(resultSEXP, i, builder.objects()[i]);
      SET_STRING_ELT(namesSEXP, i, Rf_mkChar(builder.names()[i].c_str()));
   }

   // NOTE: empty lists are unnamed
   if (n > 0)
      Rf_setAttrib(resultSEXP, R_NamesSymbol, namesSEXP);
   
   return resultSEXP;
}

SEXP createList(const std::vector<std::string>& names, Protect* pProtect)
{
   std::size_t n = names.size();
   SEXP listSEXP;
   pProtect->add(listSEXP = Rf_allocVector(VECSXP, n));

   SEXP namesSEXP;
   pProtect->add(namesSEXP = Rf_allocVector(STRSXP, n));
   for (std::size_t i = 0; i < n; ++i)
      SET_STRING_ELT(namesSEXP, i, Rf_mkChar(names[i].c_str()));

   Rf_setAttrib(listSEXP, R_NamesSymbol, namesSEXP);

   return listSEXP;
}
   
Protect::~Protect()
{
   try
   {
      unprotectAll();
   }
   catch(...)
   {
   }
}

void Protect::add(SEXP sexp)
{
   PROTECT(sexp);
   protectCount_++;
}   

void Protect::unprotectAll()
{
   if (protectCount_ > 0)
      UNPROTECT(protectCount_);
   protectCount_ = 0;
}


PreservedSEXP::PreservedSEXP()
   : sexp_(R_NilValue)
{
}

PreservedSEXP::PreservedSEXP(SEXP sexp)
   : sexp_(R_NilValue)
{
   set(sexp);
}

void PreservedSEXP::set(SEXP sexp)
{
   releaseNow();
   sexp_ = sexp ;
   if (sexp_ != R_NilValue)
      ::R_PreserveObject(sexp_);
}

PreservedSEXP::~PreservedSEXP()
{
   try
   {
      releaseNow();
   }
   catch(...)
   {
   }
}

void PreservedSEXP::releaseNow()
{
   if (sexp_ != R_NilValue)
   {
      ::R_ReleaseObject(sexp_);
      sexp_ = R_NilValue;
   }
}

void printValue(SEXP object)
{
   Error error = r::exec::executeSafely(
      boost::bind(Rf_PrintValue, object)
   );
   
   if (error)
      LOG_ERROR(error);
}

bool inherits(SEXP object, const char* S3Class)
{
   return Rf_inherits(object, S3Class);
}

std::set<std::string> makeNsePrimitives()
{
   std::set<std::string> nsePrimitives;
   nsePrimitives.insert("quote");
   nsePrimitives.insert("substitute");
   nsePrimitives.insert("match.call");
   nsePrimitives.insert("library");
   nsePrimitives.insert("require");
   nsePrimitives.insert("enquote");
   nsePrimitives.insert("bquote");
   nsePrimitives.insert("expression");
   nsePrimitives.insert("evalq");
   nsePrimitives.insert("subset");
   return nsePrimitives;
}

const std::set<std::string>& nsePrimitives()
{
   static const std::set<std::string> set = makeNsePrimitives();
   return set;
}

bool isCallToNSEFunction(SEXP node,
                         const std::set<std::string>& nsePrimitives)
{
   if (TYPEOF(node) != LANGSXP)
      return false;
   
   SEXP head = CAR(node);
   while (TYPEOF(head) == LANGSXP)
      head = CAR(head);
   
   return TYPEOF(head) == SYMSXP &&
          nsePrimitives.count(CHAR(PRINTNAME(head)));
}

// Attempts to find calls to functions which perform NSE.
bool maybePerformsNSEImpl(SEXP node,
                          const std::set<std::string>& nsePrimitives)
{
   // Check
   if (isCallToNSEFunction(node, nsePrimitives))
      return true;
   
   // Recurse
   if (TYPEOF(node) == LANGSXP)
   {
      SEXP tail = CDR(node);
      SEXP head = CAR(tail);
      while (tail != R_NilValue)
      {
         if (TYPEOF(head) == LANGSXP && maybePerformsNSEImpl(head, nsePrimitives))
            return true;
         
         tail = CDR(tail);
         head = CAR(tail);
      }
   }
   
   return false;
}

bool maybePerformsNSE(SEXP function)
{
   if (!Rf_isFunction(function))
      return false;
   
   if (Rf_isPrimitive(function))
      return false;
   
   return maybePerformsNSEImpl(
            BODY_EXPR(function),
            nsePrimitives());
}

// NOTE: Uses `R_lsInternal` which throws error if a non-environment is
// passed; we therefore perform this validation ourselves before calling
// `R_lsInternal`. This is primarily done to avoid the error being printed
// out to the R console.
SEXP objects(SEXP environment,
             bool allNames,
             Protect* pProtect)
{
   if (TYPEOF(environment) != ENVSXP)
   {
      LOG_ERROR_MESSAGE("'objects' called on non-environment");
      return R_NilValue;
   }
   
   SEXP resultSEXP;
   pProtect->add(resultSEXP = R_lsInternal(environment, allNames ? TRUE : FALSE));
   return resultSEXP;
}

Error objects(SEXP environment,
              bool allNames,
              std::vector<std::string>* pNames)
{
   Protect protect;
   SEXP objectsSEXP = objects(environment, allNames, &protect);
   
   if (Rf_isNull(objectsSEXP))
      return Error(errc::CodeExecutionError, ERROR_LOCATION);
   
   if (!fillVectorString(objectsSEXP, pNames))
      return Error(errc::CodeExecutionError, ERROR_LOCATION);
   
   return Success();
}

core::Error getNamespaceExports(SEXP ns,
                                std::vector<std::string>* pNames)
{
   r::exec::RFunction f("getNamespaceExports");
   f.addParam(ns);
   Error error = f.call(pNames);
   if (error)
      LOG_ERROR(error);
   return error;
}

core::Error extractFormalNames(SEXP functionSEXP,
                               std::vector<std::string>* pNames)
{
   if (!Rf_isFunction(functionSEXP))
      return Error(errc::UnexpectedDataTypeError, ERROR_LOCATION);
   
   // TODO: Primitives don't have formals attached to them, but
   // we could look them up in two environments in the base namespace,
   // either '.ArgsEnv' or '.GenericArgsEnv'.
   if (Rf_isPrimitive(functionSEXP))
      return Success();
   
   SEXP formals = FORMALS(functionSEXP);
   
   // Iterate through the formals pairlist and append tag names
   // to the output.
   while (formals != R_NilValue)
   {
      if (TAG(formals) != R_NilValue)
         pNames->push_back(CHAR(PRINTNAME(TAG(formals))));
      
      formals = CDR(formals);
   }
   
   return Success();
}

} // namespace sexp   
} // namespace r
} // namespace rstudio



