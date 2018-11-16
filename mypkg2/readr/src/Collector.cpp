#include <Rcpp.h>
using namespace Rcpp;

#include "Collector.h"
#include "LocaleInfo.h"
#include "QiParsers.h"

CollectorPtr Collector::create(List spec, LocaleInfo* pLocale) {
  std::string subclass(as<CharacterVector>(spec.attr("class"))[0]);

  if (subclass == "collector_skip")
    return CollectorPtr(new CollectorSkip());
  if (subclass == "collector_logical")
    return CollectorPtr(new CollectorLogical());
  if (subclass == "collector_integer")
    return CollectorPtr(new CollectorInteger());
  if (subclass == "collector_double") {
    return CollectorPtr(new CollectorDouble(pLocale->decimalMark_));
  }
  if (subclass == "collector_number")
    return CollectorPtr(
        new CollectorNumeric(pLocale->decimalMark_, pLocale->groupingMark_));
  if (subclass == "collector_character")
    return CollectorPtr(new CollectorCharacter(&pLocale->encoder_));
  if (subclass == "collector_date") {
    SEXP format_ = spec["format"];
    std::string format =
        (Rf_isNull(format_)) ? pLocale->dateFormat_ : as<std::string>(format_);
    return CollectorPtr(new CollectorDate(pLocale, format));
  }
  if (subclass == "collector_datetime") {
    std::string format = as<std::string>(spec["format"]);
    return CollectorPtr(new CollectorDateTime(pLocale, format));
  }
  if (subclass == "collector_time") {
    std::string format = as<std::string>(spec["format"]);
    return CollectorPtr(new CollectorTime(pLocale, format));
  }
  if (subclass == "collector_factor") {
    Nullable<CharacterVector> levels =
        as<Nullable<CharacterVector> >(spec["levels"]);
    bool ordered = as<bool>(spec["ordered"]);
    bool includeNa = as<bool>(spec["include_na"]);
    return CollectorPtr(
        new CollectorFactor(&pLocale->encoder_, levels, ordered, includeNa));
  }

  Rcpp::stop("Unsupported column type");
  return CollectorPtr(new CollectorSkip());
}

std::vector<CollectorPtr>
collectorsCreate(ListOf<List> specs, LocaleInfo* pLocale) {
  std::vector<CollectorPtr> collectors;
  for (int j = 0; j < specs.size(); ++j) {
    CollectorPtr col = Collector::create(specs[j], pLocale);
    collectors.push_back(col);
  }

  return collectors;
}

// Implementations ------------------------------------------------------------

void CollectorCharacter::setValue(int i, const Token& t) {
  switch (t.type()) {
  case TOKEN_STRING: {
    boost::container::string buffer;
    SourceIterators string = t.getString(&buffer);

    if (t.hasNull())
      warn(t.row(), t.col(), "", "embedded null");

    SET_STRING_ELT(
        column_,
        i,
        pEncoder_->makeSEXP(string.first, string.second, t.hasNull()));
    break;
  };
  case TOKEN_MISSING:
    SET_STRING_ELT(column_, i, NA_STRING);
    break;
  case TOKEN_EMPTY:
    SET_STRING_ELT(column_, i, Rf_mkCharCE("", CE_UTF8));
    break;
  case TOKEN_EOF:
    Rcpp::stop("Invalid token");
  }
}

void CollectorCharacter::setValue(int i, const std::string& s) {
  SET_STRING_ELT(column_, i, Rf_mkCharCE(s.c_str(), CE_UTF8));
}

void CollectorDate::setValue(int i, const Token& t) {
  switch (t.type()) {
  case TOKEN_STRING: {
    boost::container::string buffer;
    SourceIterators string = t.getString(&buffer);
    std::string std_string(string.first, string.second);

    parser_.setDate(std_string.c_str());
    bool res =
        (format_ == "") ? parser_.parseLocaleDate() : parser_.parse(format_);

    if (!res) {
      warn(t.row(), t.col(), "date like " + format_, std_string);
      REAL(column_)[i] = NA_REAL;
      return;
    }

    DateTime dt = parser_.makeDate();
    if (!dt.validDate()) {
      warn(t.row(), t.col(), "valid date", std_string);
      REAL(column_)[i] = NA_REAL;
      return;
    }
    REAL(column_)[i] = dt.date();
    return;
  }
  case TOKEN_MISSING:
  case TOKEN_EMPTY:
    REAL(column_)[i] = NA_REAL;
    return;
  case TOKEN_EOF:
    Rcpp::stop("Invalid token");
  }
}

void CollectorDateTime::setValue(int i, const Token& t) {
  switch (t.type()) {
  case TOKEN_STRING: {
    boost::container::string buffer;
    SourceIterators string = t.getString(&buffer);
    std::string std_string(string.first, string.second);

    parser_.setDate(std_string.c_str());
    bool res =
        (format_ == "") ? parser_.parseISO8601() : parser_.parse(format_);

    if (!res) {
      warn(t.row(), t.col(), "date like " + format_, std_string);
      REAL(column_)[i] = NA_REAL;
      return;
    }

    DateTime dt = parser_.makeDateTime();
    if (!dt.validDateTime()) {
      warn(t.row(), t.col(), "valid date", std_string);
      REAL(column_)[i] = NA_REAL;
      return;
    }

    REAL(column_)[i] = dt.datetime();
    return;
  }
  case TOKEN_MISSING:
  case TOKEN_EMPTY:
    REAL(column_)[i] = NA_REAL;
    return;
  case TOKEN_EOF:
    Rcpp::stop("Invalid token");
  }

  return;
}

void CollectorDouble::setValue(int i, const Token& t) {
  switch (t.type()) {
  case TOKEN_STRING: {
    boost::container::string buffer;
    SourceIterators str = t.getString(&buffer);

    bool ok =
        parseDouble(decimalMark_, str.first, str.second, REAL(column_)[i]);
    if (!ok) {
      REAL(column_)[i] = NA_REAL;
      warn(t.row(), t.col(), "a double", str);
      return;
    }

    if (str.first != str.second) {
      REAL(column_)[i] = NA_REAL;
      warn(t.row(), t.col(), "no trailing characters", str);
      return;
    }

    return;
  }
  case TOKEN_MISSING:
  case TOKEN_EMPTY:
    REAL(column_)[i] = NA_REAL;
    break;
  case TOKEN_EOF:
    Rcpp::stop("Invalid token");
  }
}

void CollectorFactor::insert(int i, Rcpp::String str, const Token& t) {
  std::map<Rcpp::String, int>::iterator it = levelset_.find(str);
  if (it == levelset_.end()) {
    if (implicitLevels_ || (includeNa_ && str == NA_STRING)) {
      int n = levelset_.size();
      levelset_.insert(std::make_pair(str, n));
      levels_.push_back(str);
      INTEGER(column_)[i] = n + 1;
    } else {
      warn(t.row(), t.col(), "value in level set", str);
      INTEGER(column_)[i] = NA_INTEGER;
    }
  } else {
    INTEGER(column_)[i] = it->second + 1;
  }
}

void CollectorFactor::setValue(int i, const Token& t) {

  switch (t.type()) {
  case TOKEN_EMPTY:
  case TOKEN_STRING: {
    boost::container::string buffer;
    SourceIterators string = t.getString(&buffer);

    Rcpp::String std_string =
        pEncoder_->makeSEXP(string.first, string.second, t.hasNull());
    insert(i, std_string, t);
    return;
  };
  case TOKEN_MISSING:
    if (includeNa_) {
      insert(i, NA_STRING, t);
    } else {
      INTEGER(column_)[i] = NA_INTEGER;
    }
    return;
  case TOKEN_EOF:
    Rcpp::stop("Invalid token");
  }
}

void CollectorInteger::setValue(int i, const Token& t) {

  switch (t.type()) {
  case TOKEN_STRING: {
    boost::container::string buffer;
    SourceIterators str = t.getString(&buffer);

    bool ok = parseInt(str.first, str.second, INTEGER(column_)[i]);
    if (!ok) {
      INTEGER(column_)[i] = NA_INTEGER;
      warn(t.row(), t.col(), "an integer", str);
      return;
    }

    if (str.first != str.second) {
      warn(t.row(), t.col(), "no trailing characters", str);
      INTEGER(column_)[i] = NA_INTEGER;
      return;
    }

    return;
  };
  case TOKEN_MISSING:
  case TOKEN_EMPTY:
    INTEGER(column_)[i] = NA_INTEGER;
    break;
  case TOKEN_EOF:
    Rcpp::stop("Invalid token");
  }
}

void CollectorLogical::setValue(int i, const Token& t) {

  switch (t.type()) {
  case TOKEN_STRING: {
    boost::container::string buffer;
    SourceIterators string = t.getString(&buffer);
    int size = string.second - string.first;

    if (Rf_StringTrue(string.first) ||
        (size == 1 && (*string.first == '1') || *string.first == 't')) {
      LOGICAL(column_)[i] = 1;
      return;
    }
    if (Rf_StringFalse(string.first) ||
        (size == 1 && (*string.first == '0') || *string.first == 'f')) {
      LOGICAL(column_)[i] = 0;
      return;
    }

    warn(t.row(), t.col(), "1/0/T/F/TRUE/FALSE", string);
    LOGICAL(column_)[i] = NA_LOGICAL;
    return;
  };
  case TOKEN_MISSING:
  case TOKEN_EMPTY:
    LOGICAL(column_)[i] = NA_LOGICAL;
    return;
    break;
  case TOKEN_EOF:
    Rcpp::stop("Invalid token");
  }
}

void CollectorNumeric::setValue(int i, const Token& t) {
  switch (t.type()) {
  case TOKEN_STRING: {
    boost::container::string buffer;
    SourceIterators str = t.getString(&buffer);

    bool ok = parseNumber(
        decimalMark_, groupingMark_, str.first, str.second, REAL(column_)[i]);

    if (!ok) {
      REAL(column_)[i] = NA_REAL;
      warn(t.row(), t.col(), "a number", str);
      return;
    }

    break;
  }
  case TOKEN_MISSING:
  case TOKEN_EMPTY:
    REAL(column_)[i] = NA_REAL;
    break;
  case TOKEN_EOF:
    Rcpp::stop("Invalid token");
  }
}

void CollectorTime::setValue(int i, const Token& t) {
  switch (t.type()) {
  case TOKEN_STRING: {
    boost::container::string buffer;
    SourceIterators string = t.getString(&buffer);
    std::string std_string(string.first, string.second);

    parser_.setDate(std_string.c_str());
    bool res =
        (format_ == "") ? parser_.parseLocaleTime() : parser_.parse(format_);

    if (!res) {
      warn(t.row(), t.col(), "time like " + format_, std_string);
      REAL(column_)[i] = NA_REAL;
      return;
    }

    DateTime dt = parser_.makeTime();
    if (!dt.validTime()) {
      warn(t.row(), t.col(), "valid date", std_string);
      REAL(column_)[i] = NA_REAL;
      return;
    }
    REAL(column_)[i] = dt.time();
    return;
  }
  case TOKEN_MISSING:
  case TOKEN_EMPTY:
    REAL(column_)[i] = NA_REAL;
    return;
  case TOKEN_EOF:
    Rcpp::stop("Invalid token");
  }
}

void CollectorRaw::setValue(int i, const Token& t) {
  if (t.type() == TOKEN_EOF) {
    Rcpp::stop("Invalid token");
  }
  SET_VECTOR_ELT(column_, i, t.asRaw());
  return;
}
