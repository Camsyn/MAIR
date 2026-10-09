#include "re2/re2.h"

#include <iostream>
#include <string>

int main() {
  int failures = 0;
  if (!RE2::FullMatch("abc", "a.c"))
    ++failures;
  if (!RE2::PartialMatch("cat dog", "(cat|dog)+"))
    ++failures;
  std::string word;
  if (!RE2::PartialMatch("one # two", "(\\w+)", &word))
    ++failures;
  RE2 invalid("[");
  if (invalid.ok())
    ++failures;
  std::cout << "word=" << word << " failures=" << failures << "\n";
  return failures == 0 ? 0 : 1;
}
