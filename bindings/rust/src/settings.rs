// MIT License © 2026 Binary Dice Games
//! Settings metadata for [`Client::describe_setting`](crate::Client::describe_setting)
//! and [`Client::describe_settings_category`](crate::Client::describe_settings_category);
//! see docs/settings.md.

/// A setting's default value.
#[derive(Clone, Debug, Default, PartialEq)]
pub enum SettingValue {
    #[default]
    None,
    Bool(bool),
    Int(i32),
    Float(f64),
    Text(String),
}

/// One choice of an enum setting.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SettingOption {
    /// Stored value.
    pub value: String,
    /// Shown text; the value when empty.
    pub label: String,
}

/// Metadata of one setting: field `color` of the user store entry
/// `bdg.desktop.example` is named `"bdg.desktop.example.color"`.
///
/// ```no_run
/// # use wish::settings::{SettingInfo, SettingValue};
/// let info = SettingInfo {
///     label: "Color".into(),
///     category: "example".into(),
///     default: SettingValue::Text("#2F7BF5".into()),
///     ..SettingInfo::new("bdg.desktop.example.color", "color")
/// };
/// ```
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SettingInfo {
    pub name: String,
    /// bool, int, float, string, enum, color or path.
    pub kind: String,
    pub label: String,
    pub description: String,
    /// Sidebar category id.
    pub category: String,
    /// Group title inside the category.
    pub section: String,
    pub default: SettingValue,
    pub min: Option<f64>,
    pub max: Option<f64>,
    pub step: Option<f64>,
    /// Choices of an enum setting.
    pub options: Vec<SettingOption>,
    pub icon: String,
    pub order: i32,
    /// A change takes effect only after a restart.
    pub restart: bool,
}

impl SettingInfo {
    /// A setting named `name` of type `kind`, with every other field empty.
    pub fn new(name: &str, kind: &str) -> Self {
        SettingInfo {
            name: name.to_string(),
            kind: kind.to_string(),
            ..Default::default()
        }
    }

    /// The JSON object `wish_settings_describe()` takes.
    pub fn to_json(&self) -> String {
        let mut j = format!(
            "{{\"name\":{},\"type\":{},\"label\":{},\"description\":{},\"category\":{},\"section\":{},\"default\":{},\"icon\":{},\"order\":{},\"restart\":{}",
            json_string(&self.name),
            json_string(&self.kind),
            json_string(&self.label),
            json_string(&self.description),
            json_string(&self.category),
            json_string(&self.section),
            json_value(&self.default),
            json_string(&self.icon),
            self.order,
            self.restart
        );
        for (key, value) in [("min", self.min), ("max", self.max), ("step", self.step)] {
            if let Some(v) = value {
                j.push_str(&format!(",\"{key}\":{}", json_number(v)));
            }
        }
        if !self.options.is_empty() {
            let options: Vec<String> = self
                .options
                .iter()
                .map(|o| {
                    format!(
                        "{{\"value\":{},\"label\":{}}}",
                        json_string(&o.value),
                        json_string(&o.label)
                    )
                })
                .collect();
            j.push_str(&format!(",\"options\":[{}]", options.join(",")));
        }
        j.push('}');
        j
    }
}

/// Metadata of one sidebar category.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SettingsCategory {
    pub id: String,
    pub label: String,
    pub description: String,
    pub icon: String,
    /// Badge color, `"#RRGGBB"`.
    pub color: String,
    pub order: i32,
}

impl SettingsCategory {
    /// The JSON object `wish_settings_describe_category()` takes.
    pub fn to_json(&self) -> String {
        format!(
            "{{\"id\":{},\"label\":{},\"description\":{},\"icon\":{},\"color\":{},\"order\":{}}}",
            json_string(&self.id),
            json_string(&self.label),
            json_string(&self.description),
            json_string(&self.icon),
            json_string(&self.color),
            self.order
        )
    }
}

fn json_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn json_number(v: f64) -> String {
    if v.is_finite() {
        format!("{v}")
    } else {
        "null".to_string()
    }
}

fn json_value(v: &SettingValue) -> String {
    match v {
        SettingValue::None => "null".to_string(),
        SettingValue::Bool(b) => b.to_string(),
        SettingValue::Int(i) => i.to_string(),
        SettingValue::Float(f) => json_number(*f),
        SettingValue::Text(s) => json_string(s),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn setting_json_has_every_field() {
        let info = SettingInfo {
            label: "Mode \"x\"".into(),
            options: vec![SettingOption {
                value: "a".into(),
                label: "Alpha".into(),
            }],
            default: SettingValue::Text("a".into()),
            min: Some(1.5),
            ..SettingInfo::new("bdg.desktop.example.mode", "enum")
        };
        let j = info.to_json();
        assert!(j.starts_with(
            "{\"name\":\"bdg.desktop.example.mode\",\"type\":\"enum\",\"label\":\"Mode \\\"x\\\"\""
        ));
        assert!(j.contains("\"default\":\"a\""));
        assert!(j.contains("\"min\":1.5"));
        assert!(j.contains("\"options\":[{\"value\":\"a\",\"label\":\"Alpha\"}]"));
        assert!(!j.contains("\"max\""));
    }

    #[test]
    fn category_json() {
        let c = SettingsCategory {
            id: "example".into(),
            color: "#AF52DE".into(),
            order: 3,
            ..Default::default()
        };
        assert_eq!(
            c.to_json(),
            "{\"id\":\"example\",\"label\":\"\",\"description\":\"\",\"icon\":\"\",\"color\":\"#AF52DE\",\"order\":3}"
        );
    }
}
