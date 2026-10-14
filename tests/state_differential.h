// Differential check of the state pre-scan against JUCE's parser: for a blob the pre-scan
// accepts, parse it exactly as setStateInformation would (getXmlFromBinary, then
// ValueTree::fromXml) and measure what JUCE actually built. Anything accepted must be
// within the limits the pre-scan claims, as JUCE sees it. Used by tests/plugin_test.cpp
// (a corpus of tricky blobs plus seeded mutants) and by the fuzz_state oracle.
#pragma once

#include "PluginProcessor.h"

#include <string>
#include <vector>

namespace differential
{
struct Parsed
{
    bool parsed = false;
    int elements = 0, maxDepth = 0, attributes = 0;
    size_t textBytes = 0;               // attribute names + values + text, as JUCE stores them
    bool colonInName = false;           // "base64:" (or any namespace) in an element/attribute name
    bool nonAsciiName = false;
    double parseMs = 0.0;
    std::string firstProblem;
};

inline Parsed parse (const void* data, int size)
{
    Parsed r;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    auto xml = juce::AudioProcessor::getXmlFromBinary (data, size);
    if (xml != nullptr)
    {
        r.parsed = true;
        // Walk without recursion: the depth being measured is the thing in question.
        std::vector<std::pair<const juce::XmlElement*, int>> stack { { xml.get(), 1 } };
        while (! stack.empty())
        {
            const auto [e, depth] = stack.back();
            stack.pop_back();
            if (e->isTextElement())
            {
                r.textBytes += (size_t) e->getText().getNumBytesAsUTF8();
                continue;
            }
            ++r.elements;
            r.maxDepth = std::max (r.maxDepth, depth);
            auto checkName = [&r] (const juce::String& n)
            {
                r.colonInName |= n.containsChar (':');
                for (auto c : n)
                    r.nonAsciiName |= c > 127;
            };
            checkName (e->getTagName());
            for (int i = 0; i < e->getNumAttributes(); ++i)
            {
                ++r.attributes;
                checkName (e->getAttributeName (i));
                r.textBytes += (size_t) e->getAttributeName (i).getNumBytesAsUTF8()
                               + (size_t) e->getAttributeValue (i).getNumBytesAsUTF8();
            }
            for (auto* child = e->getFirstChildElement(); child != nullptr; child = child->getNextElement())
                stack.push_back ({ child, depth + 1 });
        }
        (void) juce::ValueTree::fromXml (*xml); // what setStateInformation does next
    }
    r.parseMs = juce::Time::getMillisecondCounterHiRes() - start;
    return r;
}

// Empty if the accepted blob is within limits as JUCE parsed it; otherwise what isn't.
inline std::string limitsViolated (const Parsed& p)
{
    if (! p.parsed)
        return {};
    if (p.elements > FlayrTuneProcessor::kMaxXmlTags)
        return "elements " + std::to_string (p.elements);
    if (p.maxDepth > FlayrTuneProcessor::kMaxXmlTags)
        return "depth " + std::to_string (p.maxDepth);
    if (p.attributes > FlayrTuneProcessor::kMaxXmlAttributes)
        return "attributes " + std::to_string (p.attributes);
    if (p.colonInName)
        return "':' in a name (base64: decoding)";
    if (p.nonAsciiName)
        return "non-ASCII name";
    if (p.textBytes > (size_t) FlayrTuneProcessor::kMaxStateBytes)
        return "text bytes " + std::to_string (p.textBytes);
    return {};
}

// For an accepted blob: what's wrong, or empty. Refused blobs are always fine (JUCE never
// sees them).
inline std::string check (const void* data, int size)
{
    if (! FlayrTuneProcessor::isAcceptableState (data, size))
        return {};
    return limitsViolated (parse (data, size));
}
} // namespace differential
