#include "bcc/pp/identifier_table.hh"

#include "bcc/lex/token_kind.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

TEST(IdentifierTableTest, InternsToStableIdentity) {
  IdentifierTable table;

  IdentifierInfo& first = table.Get("foo");
  IdentifierInfo& second = table.Get("foo");

  EXPECT_EQ(&first, &second);
  EXPECT_EQ(first.GetName(), "foo");
}

TEST(IdentifierTableTest, DistinctSpellingsGetDistinctInfo) {
  IdentifierTable table;

  EXPECT_NE(&table.Get("foo"), &table.Get("bar"));
}

TEST(IdentifierTableTest, NameSurvivesRehash) {
  IdentifierTable table;

  // Keep a reference obtained early, then force many insertions (rehashes).
  IdentifierInfo& early = table.Get("stable_name");
  for (int i = 0; i < 1000; ++i) {
    table.Get("filler_" + std::to_string(i));
  }

  EXPECT_EQ(early.GetName(), "stable_name");
  EXPECT_EQ(&early, &table.Get("stable_name"));
}

TEST(IdentifierTableTest, PlainIdentifierIsNotAKeyword) {
  IdentifierTable table;

  IdentifierInfo& info = table.Get("my_variable");

  EXPECT_EQ(info.GetTokenKind(), TokenKind::kIdentifier);
  EXPECT_FALSE(info.IsKeyword());
  EXPECT_EQ(info.GetPPKeyword(), PPKeyword::kNotKeyword);
}

TEST(IdentifierTableTest, ClassifiesCKeywords) {
  IdentifierTable table;

  struct Case {
    std::string_view spelling;
    TokenKind kind;
  };
  const Case cases[] = {
      {"int", TokenKind::kInt},         {"auto", TokenKind::kAuto},
      {"while", TokenKind::kWhile},     {"_Bool", TokenKind::kBool},
      {"_Static_assert", TokenKind::kStaticAssert},
      {"_Thread_local", TokenKind::kThreadLocal},
  };

  for (const auto& c : cases) {
    IdentifierInfo& info = table.Get(c.spelling);
    EXPECT_EQ(info.GetTokenKind(), c.kind) << c.spelling;
    EXPECT_TRUE(info.IsKeyword()) << c.spelling;
  }
}

TEST(IdentifierTableTest, ClassifiesPreprocessorKeywords) {
  IdentifierTable table;

  // "include" is a pp-keyword but not a C keyword.
  IdentifierInfo& include = table.Get("include");
  EXPECT_EQ(include.GetPPKeyword(), PPKeyword::kInclude);
  EXPECT_EQ(include.GetTokenKind(), TokenKind::kIdentifier);
  EXPECT_FALSE(include.IsKeyword());

  EXPECT_EQ(table.Get("define").GetPPKeyword(), PPKeyword::kDefine);
  EXPECT_EQ(table.Get("defined").GetPPKeyword(), PPKeyword::kDefined);
  EXPECT_EQ(table.Get("__has_include").GetPPKeyword(),
            PPKeyword::kHasInclude);
}

TEST(IdentifierTableTest, SpellingCanBeBothCKeywordAndPPKeyword) {
  IdentifierTable table;

  // "if" is simultaneously a C keyword and a preprocessor keyword.
  IdentifierInfo& info = table.Get("if");
  EXPECT_EQ(info.GetTokenKind(), TokenKind::kIf);
  EXPECT_TRUE(info.IsKeyword());
  EXPECT_EQ(info.GetPPKeyword(), PPKeyword::kIf);
}

TEST(IdentifierTableTest, MacroDefinitionBitToggles) {
  IdentifierTable table;

  IdentifierInfo& info = table.Get("FOO");
  EXPECT_FALSE(info.HasMacroDefinition());

  info.SetHasMacroDefinition(true);
  EXPECT_TRUE(info.HasMacroDefinition());
  EXPECT_TRUE(table.Get("FOO").HasMacroDefinition());

  info.SetHasMacroDefinition(false);
  EXPECT_FALSE(table.Get("FOO").HasMacroDefinition());
}

}  // namespace
}  // namespace bcc
