#define BUILDING_NODE_EXTENSION
#include <iostream>
#include <node.h>
#include <nan.h>
#include <libxslt/xslt.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>

// includes from libxmljs
#include <xml_syntax_error.h>
#include <xml_document.h>

#include "./node_libxslt.h"
#include "./stylesheet.h"

using namespace v8;

// Assume ownership of the input document at the libxml level.
// The libxmljs object will be modified so it now represents an empty document.
static xmlDoc* stealDocument(Local<Value> input) {
    libxmljs::XmlDocument* docWrapper =
        Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(input->ToObject());
    xmlDoc* stylesheetDoc = docWrapper->xml_obj;
    xmlDoc* dummyDoc = xmlNewDoc((const xmlChar*)"1.0");
    stylesheetDoc->_private = NULL;
    dummyDoc->_private = docWrapper;
    docWrapper->xml_obj = dummyDoc;
    return stylesheetDoc;
}

NAN_METHOD(StylesheetSync) {
    Nan::HandleScope scope;
    xmlDoc* doc = stealDocument(info[0]);
    xsltStylesheetPtr stylesheet = xsltParseStylesheetDoc(doc);
    // TODO fetch actual error.
    if (!stylesheet) {
        xmlFreeDoc(doc);
        return Nan::ThrowError("Could not parse XML string as XSLT stylesheet");
    }

    Local<Object> stylesheetWrapper = Stylesheet::New(stylesheet);
  	info.GetReturnValue().Set(stylesheetWrapper);
}

// for memory the segfault i previously fixed were due to xml documents being deleted
// by garbage collector before their associated stylesheet.
class StylesheetWorker : public Nan::AsyncWorker {
 public:
  StylesheetWorker(xmlDoc* doc, Nan::Callback *callback)
    : Nan::AsyncWorker(callback), doc(doc) {}
  ~StylesheetWorker() {}

  // Executed inside the worker-thread.
  // It is not safe to access V8, or V8 data structures
  // here, so everything we need for input and output
  // should go on `this`.
  void Execute () {
    libxmljs::WorkerSentinel workerSentinel(workerParent);
    result = xsltParseStylesheetDoc(doc);
  }

  // Executed when the async work is complete
  // this function will be run inside the main event loop
  // so it is safe to use V8 again
  void HandleOKCallback () {
    Nan::HandleScope scope;
    if (!result) {
        xmlFreeDoc(doc);
        Local<Value> argv[] = { Nan::Error("Failed to parse stylesheet") };
        callback->Call(1, argv);
    } else {
        Local<Object> resultWrapper = Stylesheet::New(result);
        Local<Value> argv[] = { Nan::Null(), resultWrapper };
        callback->Call(2, argv);
    }
  };

 private:
  libxmljs::WorkerParent workerParent;
  xmlDoc* doc;
  xsltStylesheetPtr result;
};

NAN_METHOD(StylesheetAsync) {
    Nan::HandleScope scope;
    xmlDoc* doc = stealDocument(info[0]);
    Nan::Callback *callback = new Nan::Callback(info[1].As<Function>());
    StylesheetWorker* worker = new StylesheetWorker(doc, callback);
    Nan::AsyncQueueWorker(worker);
    return;
}

// duplicate from https://github.com/bsuh/node_xslt/blob/master/node_xslt.cc
void freeArray(char **array, int size) {
    for (int i = 0; i < size; i++) {
        free(array[i]);
    }
    free(array);
}
// transform a v8 array into a char** to pass params to xsl transform
// inspired by https://github.com/bsuh/node_xslt/blob/master/node_xslt.cc
char** PrepareParams(Handle<Array> array) {
    uint32_t arrayLen = array->Length();
    char** params = (char **)malloc(sizeof(char *) * (arrayLen + 1));
    memset(params, 0, sizeof(char *) * (array->Length() + 1));
    for (unsigned int i = 0; i < array->Length(); i++) {
        Local<String> param = array->Get(Nan::New<Integer>(i))->ToString();
        params[i] = (char *)malloc(sizeof(char) * (param->Utf8Length() + 1));
        param->WriteUtf8(params[i]);
    }
    return params;
}

NAN_METHOD(ApplySync) {
    Nan::HandleScope scope;
    Stylesheet* stylesheet = Nan::ObjectWrap::Unwrap<Stylesheet>(info[0]->ToObject());
    libxmljs::XmlDocument* docSource = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[1]->ToObject());
    Handle<Array> paramsArray = Handle<Array>::Cast(info[2]);
    bool outputString = info[3]->BooleanValue();

    char** params = PrepareParams(paramsArray);

    xmlDoc* result = xsltApplyStylesheet(stylesheet->stylesheet_obj, docSource->xml_obj, (const char **)params);
    if (!result) {
        freeArray(params, paramsArray->Length());
        return Nan::ThrowError("Failed to apply stylesheet");
    }

    if (outputString) {
      // As well as a libxmljs document, prepare a string result
      unsigned char* resStr;
      int len;
      xsltSaveResultToString(&resStr,&len,result,stylesheet->stylesheet_obj);
      xmlFreeDoc(result);
      info.GetReturnValue().Set(Nan::New<String>((char*)resStr).ToLocalChecked());
    } else {
      // Fill a result libxmljs document.
      // for some obscure reason I didn't manage to create a new libxmljs document in applySync,
    	// but passing a document by reference and modifying its content works fine
      // replace the empty document in docResult with the result of the stylesheet
      libxmljs::XmlDocument* docResult = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[4]->ToObject());
      docResult->xml_obj->_private = NULL;
      xmlFreeDoc(docResult->xml_obj);
      docResult->xml_obj = result;
      result->_private = docResult;
    }

    freeArray(params, paramsArray->Length());
  	return;
}

// for memory the segfault i previously fixed were due to xml documents being deleted
// by garbage collector before their associated stylesheet.
class ApplyWorker : public Nan::AsyncWorker {
 public:
  ApplyWorker(Stylesheet* stylesheet, libxmljs::XmlDocument* docSource, char** params, int paramsLength, bool outputString, libxmljs::XmlDocument* docResult, Nan::Callback *callback)
    : Nan::AsyncWorker(callback), stylesheet(stylesheet), docSource(docSource), params(params), paramsLength(paramsLength), outputString(outputString), docResult(docResult) {}
  ~ApplyWorker() {}

  // Executed inside the worker-thread.
  // It is not safe to access V8, or V8 data structures
  // here, so everything we need for input and output
  // should go on `this`.
  void Execute () {
    libxmljs::WorkerSentinel workerSentinel(workerParent);
    result = xsltApplyStylesheet(stylesheet->stylesheet_obj, docSource->xml_obj, (const char **)params);
  }

  // Executed when the async work is complete
  // this function will be run inside the main event loop
  // so it is safe to use V8 again
  void HandleOKCallback () {
    Nan::HandleScope scope;
    if (!result) {
        Local<Value> argv[] = { Nan::Error("Failed to apply stylesheet") };
        freeArray(params, paramsLength);
        callback->Call(1, argv);
        return;
    }

    if(!outputString) {
      // for some obscure reason I didn't manage to create a new libxmljs document in applySync,
      // but passing a document by reference and modifying its content works fine
      // replace the empty document in docResult with the result of the stylesheet
      docResult->xml_obj->_private = NULL;
      xmlFreeDoc(docResult->xml_obj);
      docResult->xml_obj = result;
      result->_private = docResult;
      Local<Value> argv[] = { Nan::Null() };
      freeArray(params, paramsLength);
      callback->Call(1, argv);
    } else {
      unsigned char* resStr;
      int len;
      int cnt=xsltSaveResultToString(&resStr,&len,result,stylesheet->stylesheet_obj);
      xmlFreeDoc(result);
      Local<Value> argv[] = { Nan::Null(), Nan::New<String>((char*)resStr).ToLocalChecked()};
      freeArray(params, paramsLength);
      callback->Call(2, argv);
    }


  };

 private:
  libxmljs::WorkerParent workerParent;
  Stylesheet* stylesheet;
  libxmljs::XmlDocument* docSource;
  char** params;
  int paramsLength;
  bool outputString;
  libxmljs::XmlDocument* docResult;
  xmlDoc* result;
};

NAN_METHOD(ApplyAsync) {
    Nan::HandleScope scope;

    Stylesheet* stylesheet = Nan::ObjectWrap::Unwrap<Stylesheet>(info[0]->ToObject());
    libxmljs::XmlDocument* docSource = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[1]->ToObject());
    Handle<Array> paramsArray = Handle<Array>::Cast(info[2]);
    bool outputString = info[3]->BooleanValue();

    //if (!outputString) {
    libxmljs::XmlDocument* docResult = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[4]->ToObject());
    //}

    Nan::Callback *callback = new Nan::Callback(info[5].As<Function>());

    char** params = PrepareParams(paramsArray);

    ApplyWorker* worker = new ApplyWorker(stylesheet, docSource, params, paramsArray->Length(), outputString, docResult, callback);
    for (uint32_t i = 0; i < 5; ++i) worker->SaveToPersistent(i, info[i]);
    Nan::AsyncQueueWorker(worker);
    return;
}

NAN_METHOD(RegisterEXSLT) {
    exsltRegisterAll();
    return;
}

/////// 4game

void set_string_field(Local<Object> obj, const char* name, const char* value) {
  Nan::HandleScope scope;
  if (!value) {
    return;
  }
  Nan::Set(obj, Nan::New<String>(name).ToLocalChecked(), Nan::New<String>(value, strlen(value)).ToLocalChecked());
}

void set_numeric_field(Local<Object> obj, const char* name, const int value) {
  Nan::HandleScope scope;
  Nan::Set(obj, Nan::New<String>(name).ToLocalChecked(), Nan::New<Int32>(value));
}

Local<Value> BuildSyntaxError(xmlError* error) {
  Nan::EscapableHandleScope scope;

  Local<Value> err = Exception::Error(Nan::New<String>(error->message).ToLocalChecked());
  Local<Object> out = Local<Object>::Cast(err);

  set_numeric_field(out, "domain", error->domain);
  set_numeric_field(out, "code", error->code);
  set_string_field(out, "message", error->message);
  set_numeric_field(out, "level", error->level);
  set_numeric_field(out, "column", error->int2);
  set_string_field(out, "file", error->file);
  set_numeric_field(out, "line", error->line);
  set_string_field(out, "str1", error->str1);
  set_string_field(out, "str2", error->str2);
  set_string_field(out, "str3", error->str3);

  // only add if we have something interesting
  if (error->int1) {
    set_numeric_field(out, "int1", error->int1);
  }
  return scope.Escape(err);
}

void PushToArray(void* errs, xmlError* error) {
  Nan::HandleScope scope;
  Local<Array> errors = *reinterpret_cast<Local<Array>*>(errs);
  // push method for array
  Local<Function> push = Local<Function>::Cast(errors->Get(Nan::New<String>("push").ToLocalChecked()));

  Local<Value> argv[1] = { BuildSyntaxError(error) };
  push->Call(errors, 1, argv);
}

int getXmlParserOption2(Local<Object> props, const char *key, int value) {
  Local<String> key2 = Nan::New<String>(key).ToLocalChecked();
  Local<Boolean> val = props->Get(key2)->ToBoolean();
  return val->BooleanValue() ? value : 0;
}

xmlParserOption getXmlParserOption(Local<Object> props) {
  int ret = 0;

  // http://xmlsoft.org/html/libxml-parser.html#xmlParserOption
  ret |= getXmlParserOption2(props, "recover", XML_PARSE_RECOVER); // recover on errors
  ret |= getXmlParserOption2(props, "noent", XML_PARSE_NOENT); // substitute entities
  ret |= getXmlParserOption2(props, "dtdload", XML_PARSE_DTDLOAD); // load the external subset
  ret |= getXmlParserOption2(props, "dtdattr", XML_PARSE_DTDATTR); // default DTD attributes
  ret |= getXmlParserOption2(props, "dtdvalid", XML_PARSE_DTDVALID); // validate with the DTD
  ret |= getXmlParserOption2(props, "noerror", XML_PARSE_NOERROR); // suppress error reports
  ret |= getXmlParserOption2(props, "nowarning", XML_PARSE_NOWARNING); // suppress warning reports
  ret |= getXmlParserOption2(props, "pedantic", XML_PARSE_PEDANTIC); // pedantic error reporting
  ret |= getXmlParserOption2(props, "noblanks", XML_PARSE_NOBLANKS); // remove blank nodes
  ret |= getXmlParserOption2(props, "sax1", XML_PARSE_SAX1); // use the SAX1 interface internally
  ret |= getXmlParserOption2(props, "xinclude", XML_PARSE_XINCLUDE); // Implement XInclude substitition
  ret |= getXmlParserOption2(props, "nonet", XML_PARSE_NONET); // Forbid network access
  ret |= getXmlParserOption2(props, "nodict", XML_PARSE_NODICT); // Do not reuse the context dictionnary
  ret |= getXmlParserOption2(props, "nsclean", XML_PARSE_NSCLEAN); // remove redundant namespaces declarations
  ret |= getXmlParserOption2(props, "nocdata", XML_PARSE_NOCDATA); // merge CDATA as text nodes
  ret |= getXmlParserOption2(props, "noxincnode", XML_PARSE_NOXINCNODE); // do not generate XINCLUDE START/END nodes
  ret |= getXmlParserOption2(props, "compact", XML_PARSE_COMPACT); // compact small text nodes; no modification of the tree allowed afterwards (will possibly crash if you try to modify the tree)
  ret |= getXmlParserOption2(props, "old10", XML_PARSE_OLD10); // parse using XML-1.0 before update 5
  ret |= getXmlParserOption2(props, "nobasefix", XML_PARSE_NOBASEFIX); // do not fixup XINCLUDE xml:base uris
  ret |= getXmlParserOption2(props, "huge", XML_PARSE_HUGE); // relax any hardcoded limit from the parser
  ret |= getXmlParserOption2(props, "oldsax", XML_PARSE_OLDSAX); // parse using SAX2 interface before 2.7.0
  ret |= getXmlParserOption2(props, "ignore_enc", XML_PARSE_IGNORE_ENC); // ignore internal document encoding hint
  ret |= getXmlParserOption2(props, "big_lines", XML_PARSE_BIG_LINES); // Store big lines numbers in text PSVI field

  return (xmlParserOption)ret;
}

NAN_METHOD(ReadXmlFile) {
  Nan::HandleScope scope;
  Local<Array> errors = Nan::New<Array>();
  xmlResetLastError();
  xmlSetStructuredErrorFunc(reinterpret_cast<void*>(&errors), PushToArray);
  
  xmlParserOption opts = getXmlParserOption(info[1]->ToObject());
  String::Utf8Value filename(info[0]->ToString());
  xmlDocPtr doc = xmlReadFile(*filename, NULL, opts);

  if (!doc) {
    xmlError* error = xmlGetLastError();
    if (error) {
      return Nan::ThrowError(BuildSyntaxError(error));
    }
    return Nan::ThrowError("Could not parse XML file");
  }

  Local<Object> doc_handle = libxmljs::XmlDocument::New(doc);
  Nan::Set(doc_handle, Nan::New<String>("errors").ToLocalChecked(), errors);

  xmlNode* root_node = xmlDocGetRootElement(doc);
  if (root_node == NULL) {
    return Nan::ThrowError("parsed document has no root element");
  }

  // create the xml document handle to return
  return info.GetReturnValue().Set(doc_handle);
}

NAN_METHOD(ResultToString) {
    Nan::HandleScope scope;
    libxmljs::XmlDocument* doc = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[0]->ToObject());
    Stylesheet* stylesheet = Nan::ObjectWrap::Unwrap<Stylesheet>(info[1]->ToObject());

    xmlChar *doc_ptr;
    int doc_len;
    xsltSaveResultToString(&doc_ptr, &doc_len, doc->xml_obj, stylesheet->stylesheet_obj);

    if (doc_ptr) {
        Local<String> str = Nan::New<String>((const char*)doc_ptr, doc_len).ToLocalChecked();
        xmlFree(doc_ptr);
        return info.GetReturnValue().Set(str);
    }

    return info.GetReturnValue().Set(Nan::Null());
}

/////// end 4game

// Compose the module by assigning the methods previously prepared
void InitAll(Handle<Object> exports) {
  	Stylesheet::Init(exports);
  	exports->Set(Nan::New<String>("stylesheetSync").ToLocalChecked(), Nan::New<FunctionTemplate>(StylesheetSync)->GetFunction());
    exports->Set(Nan::New<String>("stylesheetAsync").ToLocalChecked(), Nan::New<FunctionTemplate>(StylesheetAsync)->GetFunction());
  	exports->Set(Nan::New<String>("applySync").ToLocalChecked(), Nan::New<FunctionTemplate>(ApplySync)->GetFunction());
    exports->Set(Nan::New<String>("applyAsync").ToLocalChecked(), Nan::New<FunctionTemplate>(ApplyAsync)->GetFunction());
    exports->Set(Nan::New<String>("registerEXSLT").ToLocalChecked(), Nan::New<FunctionTemplate>(RegisterEXSLT)->GetFunction());
    exports->Set(Nan::New<String>("readXmlFile").ToLocalChecked(), Nan::New<FunctionTemplate>(ReadXmlFile)->GetFunction());
}
NODE_MODULE(node_libxslt, InitAll);
