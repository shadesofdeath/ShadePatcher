#pragma once
//
// taskbar_styler_rules.h - the rule language of taskbar-styler, as text.
//
// Everything here works on strings only: parsing targets, styles, style constants, the WindhawkBlur brush
// parameters and the {{...}} style variable expressions. Nothing in this header touches XAML, so it can be read
// (and reasoned about) without the shell in mind. taskbar_styler.cpp turns the parsed forms into XAML objects.
//
// The language is the one the Windhawk mod "windows-11-taskbar-styler" (m417z) documents, so the theme strings
// from the Windows 11 taskbar styling guide are understood as they are:
//
//   Target:   Type#Name[3][Prop=Value]@VisualStateGroup > ... , alternative > ...
//             `*` matches any number of intermediate parents, `:root` as the leftmost part requires a root.
//             Type names without a namespace mean Windows.UI.Xaml.Controls (Rectangle: ...Shapes.Rectangle);
//             taskbar:, systemtray:, udk: and muxc: prefixes are expanded.
//   Style:    Property=Value            a value XAML's type converter can read
//             Property:=<Xaml .../>     a XAML object; empty means "clear the property"
//             Property@State=Value      only while the target's visual state group is in that state
//             Property=>VarName         capture the property into a style variable
//             {{expr}} inside a value    a style variable expression, re-evaluated when its variables change
//             $name                     a style constant, substituted before anything else is parsed
//   Comments: a target or style starting with // is ignored.
//
#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace styler {

// ---------------------------------------------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------------------------------------------

inline std::wstring_view Trim(std::wstring_view s)
{
    const size_t first = s.find_first_not_of(L" \t\r\v\n");
    if (first == std::wstring_view::npos)
    {
        return std::wstring_view();
    }
    const size_t last = s.find_last_not_of(L" \t\r\v\n");
    return s.substr(first, last - first + 1);
}

inline std::vector<std::wstring_view> Split(std::wstring_view s, std::wstring_view delimiter)
{
    std::vector<std::wstring_view> parts;
    size_t start = 0;
    while (true)
    {
        const size_t end = s.find(delimiter, start);
        if (end == std::wstring_view::npos)
        {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, end - start));
        start = end + delimiter.size();
    }
}

inline bool StartsWith(std::wstring_view s, std::wstring_view prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool EndsWith(std::wstring_view s, std::wstring_view suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool IsComment(std::wstring_view s)
{
    return StartsWith(s, L"//");
}

inline std::wstring EscapeXmlAttribute(std::wstring_view text)
{
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t c : text)
    {
        switch (c)
        {
        case L'&':  out += L"&amp;"; break;
        case L'"':  out += L"&quot;"; break;
        case L'<':  out += L"&lt;"; break;
        case L'>':  out += L"&gt;"; break;
        default:    out += c; break;
        }
    }
    return out;
}

// Locale-independent number formatting: XAML always wants '.' as the decimal separator.
inline std::wstring FormatDoubleInvariant(double d)
{
    if (std::isnan(d))
    {
        return L"NaN";
    }
    if (std::isinf(d))
    {
        return d < 0 ? L"-Infinity" : L"Infinity";
    }
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), d);
    if (result.ec != std::errc{})
    {
        return L"0";
    }
    return std::wstring(buffer, result.ptr);
}

inline std::optional<double> ParseDoubleInvariant(std::wstring_view text)
{
    std::string narrow;
    narrow.reserve(text.size());
    for (const wchar_t c : text)
    {
        if (c > 127)
        {
            return std::nullopt;
        }
        narrow.push_back(static_cast<char>(c));
    }
    double value = 0;
    const char* first = narrow.data();
    const char* last = first + narrow.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last)
    {
        return std::nullopt;
    }
    return value;
}

inline bool IsIdentifierStart(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_';
}

inline bool IsIdentifierChar(wchar_t c)
{
    return IsIdentifierStart(c) || (c >= L'0' && c <= L'9');
}

inline bool IsValidVariableIdentifier(std::wstring_view s)
{
    if (s.empty() || !IsIdentifierStart(s[0]))
    {
        return false;
    }
    for (size_t i = 1; i < s.size(); ++i)
    {
        if (!IsIdentifierChar(s[i]))
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Style constants: `$name` in a style is replaced by the constant's value before the style is parsed.
// ---------------------------------------------------------------------------------------------------------------

using StyleConstant = std::pair<std::wstring, std::wstring>;
using StyleConstants = std::vector<StyleConstant>;

// The list is kept sorted by name length, longest first, so `$transparent` is never read as `$t` + "ransparent".
// Among names of the same length the newest comes first, so a later definition overrides an earlier one.
inline void AddStyleConstant(StyleConstants& constants, StyleConstant constant)
{
    const auto position = std::lower_bound(constants.begin(), constants.end(), constant,
        [](const StyleConstant& a, const StyleConstant& b) { return a.first.size() > b.first.size(); });
    constants.insert(position, std::move(constant));
}

inline std::wstring ApplyStyleConstants(std::wstring_view style, const StyleConstants& constants)
{
    std::wstring out;
    size_t last = 0;
    size_t pos;
    while ((pos = style.find(L'$', last)) != std::wstring_view::npos)
    {
        out.append(style.substr(last, pos - last));

        const StyleConstant* found = nullptr;
        for (const auto& constant : constants)
        {
            if (style.substr(pos + 1, constant.first.size()) == constant.first)
            {
                found = &constant;
                break;
            }
        }

        if (found)
        {
            out += found->second;
            last = pos + 1 + found->first.size();
        }
        else
        {
            out += L'$';
            last = pos + 1;
        }
    }
    out.append(style.substr(last));
    return out;
}

// `name=value`; the value may itself use constants defined before it.
inline std::optional<StyleConstant> ParseStyleConstant(std::wstring_view text, const StyleConstants& soFar)
{
    if (IsComment(text))
    {
        return std::nullopt;
    }
    const size_t eq = text.find(L'=');
    if (eq == std::wstring_view::npos)
    {
        return std::nullopt;
    }
    const std::wstring_view key = Trim(text.substr(0, eq));
    if (key.empty())
    {
        return std::nullopt;
    }
    return StyleConstant{ std::wstring(key), ApplyStyleConstants(Trim(text.substr(eq + 1)), soFar) };
}

// ---------------------------------------------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------------------------------------------

struct MatcherSpec
{
    enum class Kind
    {
        Element,    // a type, optionally with name, index, property conditions and a visual state group
        Wildcard,   // `*`: zero or more intermediate parents
        Root,       // `:root`: the next part must have no parent
    };

    Kind kind = Kind::Element;
    std::wstring type;
    std::wstring name;
    std::optional<std::wstring> visualStateGroupName;
    int oneBasedIndex = 0;
    std::vector<std::pair<std::wstring, std::wstring>> propertyValues;   // [Property=Value] conditions
};

inline std::wstring AdjustTypeName(std::wstring_view type)
{
    if (type.find_first_of(L".:") == std::wstring_view::npos)
    {
        if (type == L"Rectangle")
        {
            return L"Windows.UI.Xaml.Shapes.Rectangle";
        }
        return L"Windows.UI.Xaml.Controls." + std::wstring(type);
    }

    static const std::pair<std::wstring_view, std::wstring_view> kPrefixes[] =
    {
        { L"taskbar:",    L"Taskbar." },
        { L"systemtray:", L"SystemTray." },
        { L"udk:",        L"WindowsUdk.UI.Shell." },
        { L"muxc:",       L"Microsoft.UI.Xaml.Controls." },
    };
    for (const auto& prefix : kPrefixes)
    {
        if (StartsWith(type, prefix.first))
        {
            return std::wstring(prefix.second) + std::wstring(type.substr(prefix.first.size()));
        }
    }
    return std::wstring(type);
}

// One part of a target, e.g. `Taskbar.TaskListButton#TaskListButton[AutomationProperties.Name=Copilot]@CommonStates`.
inline MatcherSpec ParseMatcher(std::wstring_view text)
{
    MatcherSpec spec;

    const std::wstring_view trimmed = Trim(text);
    if (trimmed == L"*")
    {
        spec.kind = MatcherSpec::Kind::Wildcard;
        return spec;
    }
    if (trimmed == L":root")
    {
        spec.kind = MatcherSpec::Kind::Root;
        return spec;
    }

    size_t i = text.find_first_of(L"#@[");
    spec.type = Trim(text.substr(0, i));
    if (spec.type.empty())
    {
        throw std::runtime_error("bad target: empty type");
    }

    while (i != std::wstring_view::npos)
    {
        const size_t next = text.find_first_of(L"#@[", i + 1);
        const std::wstring_view part = text.substr(i + 1, next == std::wstring_view::npos ? std::wstring_view::npos : next - (i + 1));

        switch (text[i])
        {
        case L'#':
            if (!spec.name.empty())
            {
                throw std::runtime_error("bad target: more than one name");
            }
            spec.name = Trim(part);
            if (spec.name.empty())
            {
                throw std::runtime_error("bad target: empty name");
            }
            break;

        case L'@':
            if (spec.visualStateGroupName)
            {
                throw std::runtime_error("bad target: more than one visual state group");
            }
            spec.visualStateGroupName = std::wstring(Trim(part));
            break;

        case L'[':
        {
            std::wstring_view condition = Trim(part);
            if (condition.empty() || condition.back() != L']')
            {
                throw std::runtime_error("bad target: missing ']'");
            }
            condition = Trim(condition.substr(0, condition.size() - 1));
            if (condition.empty())
            {
                throw std::runtime_error("bad target: empty condition");
            }

            if (condition.find_first_not_of(L"0123456789") == std::wstring_view::npos)
            {
                spec.oneBasedIndex = std::stoi(std::wstring(condition));
                break;
            }

            const size_t eq = condition.find(L'=');
            if (eq == std::wstring_view::npos)
            {
                throw std::runtime_error("bad target: condition without '='");
            }
            const std::wstring_view key = Trim(condition.substr(0, eq));
            const std::wstring_view value = Trim(condition.substr(eq + 1));
            if (key.empty())
            {
                throw std::runtime_error("bad target: condition without a property name");
            }
            spec.propertyValues.emplace_back(std::wstring(key), std::wstring(value));
            break;
        }

        default:
            throw std::runtime_error("bad target");
        }

        i = next;
    }

    return spec;
}

// Splits alternatives on ',' while leaving commas inside [Property=a,b,c] alone.
inline std::vector<std::wstring_view> SplitTargetString(std::wstring_view target)
{
    std::vector<std::wstring_view> parts;
    size_t begin = 0;
    bool inCondition = false;
    for (size_t i = 0; i < target.size(); ++i)
    {
        switch (target[i])
        {
        case L'[': inCondition = true; break;
        case L']': inCondition = false; break;
        case L',':
            if (!inCondition)
            {
                parts.push_back(target.substr(begin, i - begin));
                begin = i + 1;
            }
            break;
        }
    }
    parts.push_back(target.substr(begin));
    return parts;
}

// ---------------------------------------------------------------------------------------------------------------
// Styles
// ---------------------------------------------------------------------------------------------------------------

struct ValueRule
{
    std::wstring propertyName;
    std::wstring visualState;   // empty: applies in every state
    std::wstring value;
    bool isXamlValue = false;   // `:=`

    bool IsDynamic() const { return value.find(L"{{") != std::wstring::npos; }
};

struct CaptureRule
{
    std::wstring propertyName;
    std::wstring varName;
};

struct UnresolvedRules
{
    std::vector<ValueRule> valueRules;
    std::vector<CaptureRule> captureRules;
};

// `Property[@State][:]=Value` or `Property=>VarName`.
inline std::variant<ValueRule, CaptureRule> ParseRule(std::wstring_view text)
{
    const size_t eq = text.find(L'=');
    if (eq == std::wstring_view::npos)
    {
        throw std::runtime_error("bad style: '=' is missing");
    }

    std::wstring_view name = text.substr(0, eq);
    std::wstring_view value = text.substr(eq + 1);

    if (!value.empty() && value.front() == L'>')
    {
        value = value.substr(1);
        if (!name.empty() && name.back() == L':')
        {
            throw std::runtime_error("bad style: ':=>' is not valid");
        }
        if (name.find(L'@') != std::wstring_view::npos)
        {
            throw std::runtime_error("bad style: a capture cannot have a visual state");
        }
        const std::wstring_view propertyName = Trim(name);
        const std::wstring_view varName = Trim(value);
        if (propertyName.empty())
        {
            throw std::runtime_error("bad style: empty property name");
        }
        if (!IsValidVariableIdentifier(varName))
        {
            throw std::runtime_error("bad style: invalid capture variable name");
        }
        return CaptureRule{ std::wstring(propertyName), std::wstring(varName) };
    }

    ValueRule rule;
    rule.value = Trim(value);

    if (!name.empty() && name.back() == L':')
    {
        rule.isXamlValue = true;
        name = name.substr(0, name.size() - 1);
    }

    const size_t at = name.find(L'@');
    if (at != std::wstring_view::npos)
    {
        rule.visualState = Trim(name.substr(at + 1));
        name = name.substr(0, at);
    }

    rule.propertyName = Trim(name);
    if (rule.propertyName.empty())
    {
        throw std::runtime_error("bad style: empty property name");
    }
    return rule;
}

// One alternative of a target, with its parent chain (nearest parent first) and its styles.
struct TargetRule
{
    MatcherSpec matcher;
    std::vector<MatcherSpec> parents;
    UnresolvedRules rules;
};

inline TargetRule ParseSingleTarget(std::wstring_view target, const std::vector<std::wstring>& styles)
{
    TargetRule rule;

    const std::vector<std::wstring_view> parts = Split(target, L" > ");

    bool first = true;
    bool hasVisualStateGroup = false;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
    {
        const bool isLeftmost = (it + 1 == parts.rend());
        MatcherSpec matcher = ParseMatcher(*it);

        const bool previousIsWildcard = !rule.parents.empty() && rule.parents.back().kind == MatcherSpec::Kind::Wildcard;

        switch (matcher.kind)
        {
        case MatcherSpec::Kind::Element:
            matcher.type = AdjustTypeName(matcher.type);
            break;

        case MatcherSpec::Kind::Wildcard:
            if (first)
            {
                throw std::runtime_error("bad target: '*' cannot be the styled element");
            }
            if (isLeftmost)
            {
                throw std::runtime_error("bad target: '*' cannot be the leftmost part");
            }
            if (previousIsWildcard)
            {
                throw std::runtime_error("bad target: two '*' in a row");
            }
            break;

        case MatcherSpec::Kind::Root:
            if (first)
            {
                throw std::runtime_error("bad target: ':root' cannot be the styled element");
            }
            if (!isLeftmost)
            {
                throw std::runtime_error("bad target: ':root' must be the leftmost part");
            }
            if (previousIsWildcard)
            {
                throw std::runtime_error("bad target: ':root' must be followed by a real part");
            }
            break;
        }

        if (matcher.visualStateGroupName)
        {
            if (hasVisualStateGroup)
            {
                throw std::runtime_error("bad target: more than one visual state group");
            }
            hasVisualStateGroup = true;
        }

        if (first)
        {
            for (const std::wstring& style : styles)
            {
                std::variant<ValueRule, CaptureRule> parsed = ParseRule(style);
                if (auto* valueRule = std::get_if<ValueRule>(&parsed))
                {
                    rule.rules.valueRules.push_back(std::move(*valueRule));
                }
                else
                {
                    rule.rules.captureRules.push_back(std::move(std::get<CaptureRule>(parsed)));
                }
            }
            rule.matcher = std::move(matcher);
        }
        else
        {
            rule.parents.push_back(std::move(matcher));
        }
        first = false;
    }

    return rule;
}

// ---------------------------------------------------------------------------------------------------------------
// The custom rules setting
//
// One string holds every extra rule: rules are separated by `|`, and inside a rule the target comes first with
// the styles after it, separated by `;`:
//
//     Taskbar.TaskListButton;CornerRadius=0|Rectangle#BackgroundStroke;Visibility=Collapsed
//
// A rule or style starting with // is ignored, like in the original's settings.
// ---------------------------------------------------------------------------------------------------------------

struct CustomRule
{
    std::wstring target;
    std::vector<std::wstring> styles;
};

inline std::vector<CustomRule> ParseCustomRulesSetting(std::wstring_view text)
{
    std::vector<CustomRule> rules;
    for (const std::wstring_view ruleText : Split(text, L"|"))
    {
        const std::vector<std::wstring_view> parts = Split(ruleText, L";");
        const std::wstring_view target = Trim(parts[0]);
        if (target.empty() || IsComment(target))
        {
            continue;
        }
        CustomRule rule;
        rule.target = std::wstring(target);
        for (size_t i = 1; i < parts.size(); ++i)
        {
            const std::wstring_view style = Trim(parts[i]);
            if (!style.empty() && !IsComment(style))
            {
                rule.styles.emplace_back(style);
            }
        }
        if (!rule.styles.empty())
        {
            rules.push_back(std::move(rule));
        }
    }
    return rules;
}

// ---------------------------------------------------------------------------------------------------------------
// WindhawkBlur
//
//     <WindhawkBlur BlurAmount="18" TintColor="#25323232" TintOpacity="0.5" TintLuminosityOpacity="1.0"
//                   TintSaturation="1.0" NoiseOpacity="0.1" NoiseDensity="1.0" FallbackColor="#FF202020"/>
//
// TintColor and FallbackColor may also be `{ThemeResource Key}`, which the brush resolves against the element.
// ---------------------------------------------------------------------------------------------------------------

struct BlurColor
{
    uint8_t a = 255;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

struct BlurParams
{
    float blurAmount = 0;
    BlurColor tint;
    std::optional<uint8_t> tintOpacity;
    std::wstring tintThemeResourceKey;
    std::optional<float> tintLuminosityOpacity;
    std::optional<float> tintSaturation;
    std::optional<float> noiseOpacity;
    std::optional<float> noiseDensity;
    std::optional<BlurColor> fallbackColor;
    std::wstring fallbackThemeResourceKey;
};

inline BlurColor ParseHexColor(std::wstring_view hex)
{
    bool hasAlpha;
    switch (hex.size())
    {
    case 6: hasAlpha = false; break;
    case 8: hasAlpha = true; break;
    default: throw std::runtime_error("WindhawkBlur: a color must be #RRGGBB or #AARRGGBB");
    }
    const unsigned long value = std::stoul(std::wstring(hex), nullptr, 16);
    BlurColor color;
    color.a = hasAlpha ? static_cast<uint8_t>(value >> 24) : 255;
    color.r = static_cast<uint8_t>(value >> 16);
    color.g = static_cast<uint8_t>(value >> 8);
    color.b = static_cast<uint8_t>(value);
    return color;
}

// nullopt when the text is not a WindhawkBlur element at all; throws when it is one but malformed.
inline std::optional<BlurParams> ParseWindhawkBlur(std::wstring_view text)
{
    std::wstring_view body = Trim(text);
    if (!StartsWith(body, L"<WindhawkBlur "))
    {
        return std::nullopt;
    }
    body = body.substr(14);
    if (!EndsWith(body, L"/>"))
    {
        throw std::runtime_error("WindhawkBlur: must end with '/>'");
    }
    body = body.substr(0, body.size() - 2);

    BlurParams params;
    float tintOpacity = std::numeric_limits<float>::quiet_NaN();
    bool pendingTintResource = false;
    bool pendingFallbackResource = false;

    auto quotedValue = [](std::wstring_view part, std::wstring_view prefix, std::wstring_view* out) -> bool
    {
        if (StartsWith(part, prefix) && part.back() == L'"' && part.size() > prefix.size())
        {
            *out = part.substr(prefix.size(), part.size() - prefix.size() - 1);
            return true;
        }
        return false;
    };

    for (const std::wstring_view rawPart : Split(body, L" "))
    {
        const std::wstring_view part = Trim(rawPart);
        if (part.empty())
        {
            continue;
        }

        if (pendingTintResource)
        {
            if (!EndsWith(part, L"}\""))
            {
                throw std::runtime_error("WindhawkBlur: bad TintColor theme resource");
            }
            params.tintThemeResourceKey = std::wstring(part.substr(0, part.size() - 2));
            pendingTintResource = false;
            continue;
        }
        if (pendingFallbackResource)
        {
            if (!EndsWith(part, L"}\""))
            {
                throw std::runtime_error("WindhawkBlur: bad FallbackColor theme resource");
            }
            params.fallbackThemeResourceKey = std::wstring(part.substr(0, part.size() - 2));
            pendingFallbackResource = false;
            continue;
        }

        if (part == L"TintColor=\"{ThemeResource")
        {
            pendingTintResource = true;
            continue;
        }
        if (part == L"FallbackColor=\"{ThemeResource")
        {
            pendingFallbackResource = true;
            continue;
        }

        std::wstring_view value;
        if (quotedValue(part, L"TintColor=\"#", &value))
        {
            params.tint = ParseHexColor(value);
        }
        else if (quotedValue(part, L"FallbackColor=\"#", &value))
        {
            params.fallbackColor = ParseHexColor(value);
        }
        else if (quotedValue(part, L"TintOpacity=\"", &value))
        {
            tintOpacity = std::stof(std::wstring(value));
        }
        else if (quotedValue(part, L"TintLuminosityOpacity=\"", &value))
        {
            params.tintLuminosityOpacity = std::stof(std::wstring(value));
        }
        else if (quotedValue(part, L"TintSaturation=\"", &value))
        {
            params.tintSaturation = std::stof(std::wstring(value));
        }
        else if (quotedValue(part, L"NoiseOpacity=\"", &value))
        {
            params.noiseOpacity = std::stof(std::wstring(value));
        }
        else if (quotedValue(part, L"NoiseDensity=\"", &value))
        {
            params.noiseDensity = std::stof(std::wstring(value));
        }
        else if (quotedValue(part, L"BlurAmount=\"", &value))
        {
            params.blurAmount = std::stof(std::wstring(value));
        }
        else
        {
            throw std::runtime_error("WindhawkBlur: unknown property");
        }
    }

    if (pendingTintResource || pendingFallbackResource)
    {
        throw std::runtime_error("WindhawkBlur: unterminated theme resource");
    }

    if (!std::isnan(tintOpacity))
    {
        tintOpacity = std::clamp(tintOpacity, 0.0f, 1.0f);
        params.tint.a = static_cast<uint8_t>(tintOpacity * 255.0f);
        params.tintOpacity = params.tint.a;
    }

    return params;
}

// ---------------------------------------------------------------------------------------------------------------
// Style variable expressions: {{ ... }}
//
// Operands: numbers, `strings` (a doubled backtick is one backtick), variable names, parentheses.
// Operators: + - * / with unary sign, < <= == >= > !=, cond ? a : b, min(a, b), max(a, b), skip().
// Numbers and strings are distinct: arithmetic and ordering want numbers, == and != compare like with like and
// call a number and a string unequal, the conditional wants a numeric condition. skip() leaves the style
// unapplied. A variable that is not defined reads as the empty string inside an expression; a bare {{Var}}
// substitutes the captured text as it is, and skips the style when the variable is undefined or not a
// primitive.
// ---------------------------------------------------------------------------------------------------------------

struct VariableValue
{
    std::wstring text;               // the captured value as text
    std::optional<double> number;    // set when the captured value was numeric
    bool substitutable = false;      // false for opaque values (brushes and the like)
};

// Returns the value of a variable, or nullopt when it is not defined. Called for every reference, so the caller
// can record which variables an expression depends on.
using VariableLookup = std::function<std::optional<VariableValue>(std::wstring_view name)>;

// Thrown by skip(): the style is to be left alone. Not a std::exception on purpose.
struct SkipRequested {};

class ExpressionEvaluator
{
public:
    ExpressionEvaluator(std::wstring_view text, const VariableLookup& lookup) : m_text(text), m_lookup(lookup) {}

    std::wstring Evaluate()
    {
        Value value = ParseConditional();
        SkipSpaces();
        if (m_pos != m_text.size())
        {
            throw std::runtime_error("unexpected text after the expression");
        }
        return value.isNumber ? FormatDoubleInvariant(value.number) : value.text;
    }

private:
    struct Value
    {
        bool isNumber = false;
        double number = 0;
        std::wstring text;

        static Value Number(double d) { Value v; v.isNumber = true; v.number = d; return v; }
        static Value String(std::wstring s) { Value v; v.text = std::move(s); return v; }
    };

    void SkipSpaces()
    {
        while (m_pos < m_text.size() && (m_text[m_pos] == L' ' || m_text[m_pos] == L'\t'))
        {
            ++m_pos;
        }
    }

    bool Peek(std::wstring_view token)
    {
        SkipSpaces();
        return StartsWith(m_text.substr(m_pos), token);
    }

    bool Accept(std::wstring_view token)
    {
        if (Peek(token))
        {
            m_pos += token.size();
            return true;
        }
        return false;
    }

    void Expect(std::wstring_view token)
    {
        if (!Accept(token))
        {
            throw std::runtime_error("expected token missing");
        }
    }

    double RequireNumber(const Value& value) const
    {
        if (!value.isNumber)
        {
            throw std::runtime_error("a number was expected");
        }
        return value.number;
    }

    Value ParseConditional()
    {
        Value condition = ParseComparison();
        if (!Accept(L"?"))
        {
            return condition;
        }

        // Only the branch that is taken is evaluated, so a skip() in the other one does nothing.
        const bool taken = m_active ? (RequireNumber(condition) != 0) : true;

        const bool wasActive = m_active;
        m_active = wasActive && taken;
        Value whenTrue = ParseConditional();
        Expect(L":");
        m_active = wasActive && !taken;
        Value whenFalse = ParseConditional();
        m_active = wasActive;

        return taken ? whenTrue : whenFalse;
    }

    Value ParseComparison()
    {
        Value left = ParseAdditive();
        while (true)
        {
            std::wstring_view op;
            if (Peek(L"<="))      op = L"<=";
            else if (Peek(L">=")) op = L">=";
            else if (Peek(L"==")) op = L"==";
            else if (Peek(L"!=")) op = L"!=";
            else if (Peek(L"<"))  op = L"<";
            else if (Peek(L">"))  op = L">";
            else break;
            m_pos += op.size();

            Value right = ParseAdditive();
            if (!m_active)
            {
                continue;
            }

            bool result;
            if (op == L"==" || op == L"!=")
            {
                bool equal;
                if (left.isNumber != right.isNumber)
                {
                    equal = false;
                }
                else if (left.isNumber)
                {
                    equal = left.number == right.number;
                }
                else
                {
                    equal = left.text == right.text;
                }
                result = (op == L"==") ? equal : !equal;
            }
            else
            {
                const double a = RequireNumber(left);
                const double b = RequireNumber(right);
                if (op == L"<")       result = a < b;
                else if (op == L"<=") result = a <= b;
                else if (op == L">")  result = a > b;
                else                  result = a >= b;
            }
            left = Value::Number(result ? 1.0 : 0.0);
        }
        return left;
    }

    Value ParseAdditive()
    {
        Value left = ParseMultiplicative();
        while (true)
        {
            wchar_t op;
            if (Accept(L"+"))      op = L'+';
            else if (Accept(L"-")) op = L'-';
            else break;

            Value right = ParseMultiplicative();
            if (!m_active)
            {
                continue;
            }
            const double a = RequireNumber(left);
            const double b = RequireNumber(right);
            left = Value::Number(op == L'+' ? a + b : a - b);
        }
        return left;
    }

    Value ParseMultiplicative()
    {
        Value left = ParseUnary();
        while (true)
        {
            wchar_t op;
            if (Accept(L"*"))      op = L'*';
            else if (Accept(L"/")) op = L'/';
            else break;

            Value right = ParseUnary();
            if (!m_active)
            {
                continue;
            }
            const double a = RequireNumber(left);
            const double b = RequireNumber(right);
            if (op == L'/' && b == 0)
            {
                throw std::runtime_error("division by zero");
            }
            left = Value::Number(op == L'*' ? a * b : a / b);
        }
        return left;
    }

    Value ParseUnary()
    {
        if (Accept(L"-"))
        {
            Value operand = ParseUnary();
            return m_active ? Value::Number(-RequireNumber(operand)) : operand;
        }
        if (Accept(L"+"))
        {
            Value operand = ParseUnary();
            return m_active ? Value::Number(RequireNumber(operand)) : operand;
        }
        return ParsePrimary();
    }

    Value ParsePrimary()
    {
        SkipSpaces();
        if (m_pos >= m_text.size())
        {
            throw std::runtime_error("unexpected end of expression");
        }

        const wchar_t c = m_text[m_pos];

        if (c == L'(')
        {
            ++m_pos;
            Value inner = ParseConditional();
            Expect(L")");
            return inner;
        }

        if (c == L'`')
        {
            ++m_pos;
            std::wstring text;
            while (true)
            {
                if (m_pos >= m_text.size())
                {
                    throw std::runtime_error("unterminated string literal");
                }
                if (m_text[m_pos] == L'`')
                {
                    if (m_pos + 1 < m_text.size() && m_text[m_pos + 1] == L'`')
                    {
                        text += L'`';
                        m_pos += 2;
                        continue;
                    }
                    ++m_pos;
                    break;
                }
                text += m_text[m_pos++];
            }
            return Value::String(std::move(text));
        }

        if ((c >= L'0' && c <= L'9') || c == L'.')
        {
            const size_t start = m_pos;
            while (m_pos < m_text.size() && ((m_text[m_pos] >= L'0' && m_text[m_pos] <= L'9') || m_text[m_pos] == L'.'))
            {
                ++m_pos;
            }
            if (m_pos < m_text.size() && (m_text[m_pos] == L'e' || m_text[m_pos] == L'E'))
            {
                size_t p = m_pos + 1;
                if (p < m_text.size() && (m_text[p] == L'+' || m_text[p] == L'-'))
                {
                    ++p;
                }
                if (p < m_text.size() && m_text[p] >= L'0' && m_text[p] <= L'9')
                {
                    while (p < m_text.size() && m_text[p] >= L'0' && m_text[p] <= L'9')
                    {
                        ++p;
                    }
                    m_pos = p;
                }
            }
            const std::optional<double> number = ParseDoubleInvariant(m_text.substr(start, m_pos - start));
            if (!number)
            {
                throw std::runtime_error("bad number");
            }
            return Value::Number(*number);
        }

        if (IsIdentifierStart(c))
        {
            const size_t start = m_pos;
            while (m_pos < m_text.size() && IsIdentifierChar(m_text[m_pos]))
            {
                ++m_pos;
            }
            const std::wstring_view name = m_text.substr(start, m_pos - start);

            if (Accept(L"("))
            {
                if (name == L"skip")
                {
                    Expect(L")");
                    if (m_active)
                    {
                        throw SkipRequested{};
                    }
                    return Value::Number(0);
                }
                if (name == L"min" || name == L"max")
                {
                    Value a = ParseConditional();
                    Expect(L",");
                    Value b = ParseConditional();
                    Expect(L")");
                    if (!m_active)
                    {
                        return Value::Number(0);
                    }
                    const double x = RequireNumber(a);
                    const double y = RequireNumber(b);
                    return Value::Number(name == L"min" ? std::min(x, y) : std::max(x, y));
                }
                throw std::runtime_error("unknown function");
            }

            // A variable. Looked up even on the branch not taken, so the dependency is recorded either way.
            const std::optional<VariableValue> value = m_lookup(name);
            if (!value)
            {
                return Value::String(std::wstring());
            }
            if (!value->substitutable)
            {
                if (m_active)
                {
                    throw std::runtime_error("the variable holds a value that cannot be used in an expression");
                }
                return Value::String(std::wstring());
            }
            if (value->number)
            {
                return Value::Number(*value->number);
            }
            return Value::String(value->text);
        }

        throw std::runtime_error("unexpected character");
    }

    std::wstring_view m_text;
    const VariableLookup& m_lookup;
    size_t m_pos = 0;
    bool m_active = true;
};

// The text of one {{...}} substitution. nullopt when the style is to be skipped (undefined bare variable, opaque
// value, or an error, which is described in *error). SkipRequested propagates.
inline std::optional<std::wstring> EvaluateExpression(std::wstring_view text, const VariableLookup& lookup, std::wstring* error)
{
    const std::wstring_view trimmed = Trim(text);
    if (trimmed.empty())
    {
        *error = L"empty expression";
        return std::nullopt;
    }

    if (IsValidVariableIdentifier(trimmed))
    {
        const std::optional<VariableValue> value = lookup(trimmed);
        if (!value)
        {
            *error = L"variable '" + std::wstring(trimmed) + L"' is not defined yet";
            return std::nullopt;
        }
        if (!value->substitutable)
        {
            *error = L"variable '" + std::wstring(trimmed) + L"' is not a primitive value";
            return std::nullopt;
        }
        return value->text;
    }

    try
    {
        ExpressionEvaluator evaluator(trimmed, lookup);
        return evaluator.Evaluate();
    }
    catch (SkipRequested const&)
    {
        throw;
    }
    catch (std::exception const& ex)
    {
        std::string what = ex.what();
        *error = std::wstring(what.begin(), what.end()) + L" in '" + std::wstring(trimmed) + L"'";
        return std::nullopt;
    }
}

// Expands every {{...}} in a style value, innermost first: the first `}}` pairs with the nearest `{{` before it,
// so `{{{x}}}` gives `{` + x + `}`. Substituted text is not expanded again.
inline std::optional<std::wstring> ExpandStyleVariables(std::wstring_view input, const VariableLookup& lookup, std::wstring* error)
{
    std::wstring result(input);
    size_t scanFrom = 0;

    while (true)
    {
        size_t close = std::wstring::npos;
        for (size_t i = scanFrom; i + 1 < result.size(); ++i)
        {
            if (result[i] == L'}' && result[i + 1] == L'}')
            {
                close = i;
                break;
            }
        }
        if (close == std::wstring::npos)
        {
            break;
        }

        size_t open = std::wstring::npos;
        for (size_t j = close; j >= 2; --j)
        {
            if (result[j - 2] == L'{' && result[j - 1] == L'{')
            {
                open = j - 2;
                break;
            }
        }
        if (open == std::wstring::npos)
        {
            *error = L"'}}' without a matching '{{'";
            return std::nullopt;
        }

        const std::wstring_view expression(result.data() + open + 2, close - open - 2);
        const std::optional<std::wstring> expanded = EvaluateExpression(expression, lookup, error);
        if (!expanded)
        {
            return std::nullopt;
        }

        result.replace(open, close + 2 - open, *expanded);
        scanFrom = open + expanded->size();
    }

    return result;
}

}   // namespace styler
