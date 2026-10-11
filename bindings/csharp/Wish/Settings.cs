// MIT License © 2026 Binary Dice Games
/**
 * @file Settings.cs
 * @brief Settings metadata for `Client.DescribeSetting()` /
 *        `Client.DescribeSettingsCategory()` (see docs/settings.md).
 */

using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Bdg.Wish;

/// <summary>One choice of an enum setting.</summary>
public sealed record SettingOption(
    [property: JsonPropertyName("value")] string Value,
    [property: JsonPropertyName("label")] string Label = "");

/// <summary>
/// Metadata of one setting: field <c>color</c> of the user store entry
/// <c>bdg.desktop.example</c> is named <c>"bdg.desktop.example.color"</c>.
/// </summary>
public sealed class SettingInfo
{
    [JsonPropertyName("name")] public string Name { get; set; } = "";

    /// <summary>bool, int, float, string, enum, color or path.</summary>
    [JsonPropertyName("type")] public string Type { get; set; } = "string";

    [JsonPropertyName("label")] public string Label { get; set; } = "";
    [JsonPropertyName("description")] public string Description { get; set; } = "";

    /// <summary>Sidebar category id.</summary>
    [JsonPropertyName("category")] public string Category { get; set; } = "";

    /// <summary>Group title inside the category.</summary>
    [JsonPropertyName("section")] public string Section { get; set; } = "";

    /// <summary>null, bool, int, double or string.</summary>
    [JsonPropertyName("default")] public object? Default { get; set; }

    [JsonPropertyName("min"), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public double? Min { get; set; }

    [JsonPropertyName("max"), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public double? Max { get; set; }

    [JsonPropertyName("step"), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public double? Step { get; set; }

    /// <summary>Choices of an enum setting.</summary>
    [JsonPropertyName("options"), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<SettingOption>? Options { get; set; }

    [JsonPropertyName("icon")] public string Icon { get; set; } = "";
    [JsonPropertyName("order")] public int Order { get; set; }

    /// <summary>A change takes effect only after a restart.</summary>
    [JsonPropertyName("restart")] public bool Restart { get; set; }

    /// <summary>The JSON object <c>wish_settings_describe()</c> takes.</summary>
    public string ToJson() => JsonSerializer.Serialize(this);
}

/// <summary>Metadata of one sidebar category.</summary>
public sealed class SettingsCategory
{
    [JsonPropertyName("id")] public string Id { get; set; } = "";
    [JsonPropertyName("label")] public string Label { get; set; } = "";
    [JsonPropertyName("description")] public string Description { get; set; } = "";
    [JsonPropertyName("icon")] public string Icon { get; set; } = "";

    /// <summary>Badge color, "#RRGGBB".</summary>
    [JsonPropertyName("color")] public string Color { get; set; } = "";

    [JsonPropertyName("order")] public int Order { get; set; }

    /// <summary>The JSON object <c>wish_settings_describe_category()</c> takes.</summary>
    public string ToJson() => JsonSerializer.Serialize(this);
}
