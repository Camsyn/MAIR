#include <json/json.h>

#include <iostream>
#include <memory>
#include <string>

namespace {

bool parse(const std::string &Text, Json::Value *Root) {
  Json::CharReaderBuilder Builder;
  const std::unique_ptr<Json::CharReader> Reader(Builder.newCharReader());
  std::string Errors;
  return Reader->parse(Text.data(), Text.data() + Text.size(), Root, &Errors);
}

} // namespace

int main() {
  int Failures = 0;
  Json::Value Root;
  const std::string Document = "{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{}}";
  if (!parse(Document, &Root))
    ++Failures;
  if (!Root.isObject() || !Root.isMember("a") || !Root["a"].isInt() ||
      Root["a"].asInt() != 1)
    ++Failures;
  if (!Root["b"].isArray() || Root["b"].size() != 3 || !Root["b"][0].isBool() ||
      !Root["b"][0].asBool())
    ++Failures;
  if (!Root["b"][1].isNull() || !Root["b"][2].isString() ||
      Root["b"][2].asString() != "x")
    ++Failures;
  if (!Root["c"].isObject())
    ++Failures;
  Json::Value Invalid;
  if (parse("{", &Invalid))
    ++Failures;
  std::cout << "failures=" << Failures << "\n";
  return Failures == 0 ? 0 : 1;
}
