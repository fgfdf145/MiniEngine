#pragma once

#include <optional>
#include <string_view>
#include <vector>

// The little of XML that SvgIcon and SvgFont read: elements by name and their attributes, as text.
// No entities are expanded and no nesting is followed.
namespace me::svg
{
// The value of attribute `name` in the tag text `tag` ("<path d='...' ...>"), if present.
std::optional<std::string_view> Attribute(std::string_view tag, std::string_view name);

// The text of each element `<name ...>` in the document, up to its closing '>'.
std::vector<std::string_view> Tags(std::string_view document, std::string_view name);

// The first number in `text`, as SVG writes numbers.
bool ParseNumber(std::string_view text, float& value);
}
