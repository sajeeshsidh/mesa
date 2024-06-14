// Copyright © 2024 Igalia S.L.
// SPDX-License-Identifier: MIT

extern crate proc_macro;
extern crate proc_macro2;
extern crate quote;
extern crate roxmltree;
extern crate syn;

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::quote;
use quote::ToTokens;
use roxmltree::Document;
use std::fs;
use std::path::Path;
use syn::{parse_macro_input, parse_quote, Attribute, DeriveInput, Expr, ExprLit, Lit, Meta};

mod isa;

/// Parses the derive input to extract file paths from attributes
///
/// # Arguments
/// * `ast` - A reference to the `DeriveInput` syntax tree node
///
/// # Returns
/// A tuple containing the paths to the ISA and static rules files
///
/// # Panics
/// Panics if the necessary attributes are not found or are in the wrong format
pub(crate) fn parse_derive(ast: &DeriveInput) -> (String, String) {
    // Collect attributes with the name "isa"
    let isa_attrs: Vec<&Attribute> = ast
        .attrs
        .iter()
        .filter(|attr| {
            let path = attr.meta.path();
            path.is_ident("isa")
        })
        .collect();

    // Ensure there is at least one "isa" attribute
    if isa_attrs.is_empty() {
        panic!("An ISA file needs to be provided with the #[isa = \"PATH\"] attribute");
    }

    // Get the path from the "isa" attribute
    let isa_path = get_attribute(isa_attrs[0]);

    // Collect attributes with the name "static_rules_file"
    let static_rules_attrs: Vec<&Attribute> = ast
        .attrs
        .iter()
        .filter(|attr| {
            let path = attr.meta.path();
            path.is_ident("static_rules_file")
        })
        .collect();

    // Ensure there is at least one "static_rules_file" attribute
    if static_rules_attrs.is_empty() {
        panic!("A static pest rules file needs to be provided with the #[static_rules_file = \"PATH\"] attribute");
    }

    // Get the path from the "static_rules_file" attribute
    let static_rules_path = get_attribute(static_rules_attrs[0]);

    (isa_path, static_rules_path)
}

/// Extracts the string value from a name-value attribute
///
/// # Arguments
/// * `attr` - A reference to the `Attribute`
///
/// # Returns
/// The string value of the attribute
///
/// # Panics
/// Panics if the attribute is not in the expected format
fn get_attribute(attr: &Attribute) -> String {
    match &attr.meta {
        Meta::NameValue(name_value) => match &name_value.value {
            Expr::Lit(ExprLit {
                lit: Lit::Str(string),
                ..
            }) => {
                if name_value.path.is_ident("isa") || name_value.path.is_ident("static_rules_file")
                {
                    string.value()
                } else {
                    panic!("Attribute must be a file path")
                }
            }
            _ => panic!("Attribute must be a string"),
        },
        _ => panic!("Attribute must be of the form `key = \"...\"`"),
    }
}

/// Generates the `FromPestRule` trait
///
/// # Returns
/// A `TokenStream2` containing the trait definition
fn generate_from_rule_trait() -> TokenStream2 {
    quote! {
        pub trait FromPestRule {
            fn from_rule(rule: Rule) -> Self where Self: Sized;
        }
    }
}

/// Formats an enum value as a string in uppercase with underscores
///
/// # Arguments
/// * `enum_name` - The name of the enum
/// * `enum_value` - The value of the enum
///
/// # Returns
/// A formatted string
fn format_enum_value_str(enum_name: &str, enum_value: &str) -> String {
    format!("{}_{}", enum_name, enum_value.replace(['.', '[', ']'], "")).to_ascii_uppercase()
}

/// Retrieves and formats the enum value string from a `BitSetEnumValue`
///
/// # Arguments
/// * `enum_name` - The name of the enum
/// * `enum_value` - A reference to the `BitSetEnumValue`
///
/// # Returns
/// A formatted string
fn get_enum_value_str(enum_name: &str, enum_value: &isa::BitSetEnumValue) -> String {
    let str = if let Some(name) = enum_value.name {
        name
    } else {
        enum_value.display
    };

    format_enum_value_str(enum_name, str)
}

/// Generates the implementation of `FromPestRule` for enums in the ISA
///
/// # Arguments
/// * `isa` - A reference to the `ISA` struct
///
/// # Returns
/// A `TokenStream2` containing the implementations
fn generate_from_rule_impl_enums(isa: &isa::ISA) -> TokenStream2 {
    isa.enums
        .iter()
        .map(|e| {
            let enum_name_str = format!("isa_{}", e.name.trim_start_matches('#'));

            let enum_name = syn::Ident::new(enum_name_str.as_str(), proc_macro2::Span::call_site());
            let match_arms: Vec<_> = e
                .values
                .iter()
                .filter(|v| !v.display.is_empty() && v.display != ".____")
                .map(|v| {
                    let variant_name = syn::Ident::new(
                        get_enum_value_str(&enum_name_str, v).as_str(),
                        proc_macro2::Span::call_site(),
                    );
                    let rule_name = syn::Ident::new(
                        &to_upper_camel_case(v.name.unwrap_or(v.display), false),
                        proc_macro2::Span::call_site(),
                    );
                    quote! { Rule::#rule_name => #enum_name::#variant_name }
                })
                .collect();

            quote! {
                impl FromPestRule for #enum_name {
                    fn from_rule(rule: Rule) -> Self where Self: Sized {
                        match rule {
                            #(#match_arms),*,
                            _ => panic!("Unexpected rule: {:?}", rule),
                        }
                    }
                }
            }
        })
        .collect()
}

/// Generates the implementation of `FromPestRule` for ISA opcodes
///
/// # Arguments
/// * `isa` - A reference to the `ISA` struct
///
/// # Returns
/// A `TokenStream2` containing the implementation
fn generate_from_rule_impl_opc(isa: &isa::ISA) -> TokenStream2 {
    let instr_name = syn::Ident::new("isa_opc", proc_macro2::Span::call_site());

    let match_arms: Vec<_> = isa
        .bitsets
        .iter()
        .filter(|bitset| !bitset.name.starts_with('#'))
        .map(|instr| {
            let variant_name = syn::Ident::new(
                format_enum_value_str("isa_opc", instr.name).as_str(),
                proc_macro2::Span::call_site(),
            );

            let pest_rule = format!("Opc_{}", instr.name);

            let rule_name = syn::Ident::new(
                &to_upper_camel_case(pest_rule.as_str(), true),
                proc_macro2::Span::call_site(),
            );
            quote! { Rule::#rule_name => #instr_name::#variant_name }
        })
        .collect();

    quote! {
        impl FromPestRule for isa_opc {
            fn from_rule(rule: Rule) -> Self where Self: Sized {
                match rule {
                    #(#match_arms),*,
                    _ => panic!("Unexpected rule: {:?}", rule),
                }
            }
        }
    }
}

/// Main derive function to generate the parser
///
/// # Arguments
/// * `input` - The input token stream
///
/// # Returns
/// The output token stream
fn derive_parser(input: TokenStream) -> TokenStream {
    let mut ast: DeriveInput = parse_macro_input!(input as DeriveInput);
    let root = "../src/etnaviv/isa/";
    let (isa_filename, static_rules_filename) = parse_derive(&ast);
    let isa_path = Path::new(&root).join(isa_filename);
    let static_rules_path = Path::new(&root).join(static_rules_filename);

    // Load the XML document
    let xml_content = fs::read_to_string(isa_path).expect("Failed to read XML file");
    let doc = Document::parse(&xml_content).expect("Failed to parse XML");
    let isa = isa::ISA::new(&doc);

    // Load the static rules
    let static_rules =
        fs::read_to_string(static_rules_path).expect("Failed to read static rules pest file");
    let mut grammar = static_rules;

    // Append generated grammar rules
    grammar.push_str(&generate_peg_grammar(&isa));

    // Add grammar as an attribute to the AST
    ast.attrs.push(parse_quote! {
        #[grammar_inline = #grammar]
    });

    // Generate the token streams for the parser, trait, and rule implementations
    let tokens_parser = pest_generator::derive_parser(ast.to_token_stream(), false);
    let tokens_trait = generate_from_rule_trait();
    let tokens_from_rule_enums = generate_from_rule_impl_enums(&isa);
    let tokens_from_rule_opc = generate_from_rule_impl_opc(&isa);

    // Combine all token streams into one
    let tokens = quote! {
        #tokens_parser
        #tokens_trait
        #tokens_from_rule_enums
        #tokens_from_rule_opc
    };

    tokens.into()
}

/// Generates PEG grammar rules for enums
///
/// # Arguments
/// * `isa` - A reference to the `ISA` struct
///
/// # Returns
/// A string containing the PEG grammar rules for enums
fn generate_peg_grammar_enums(isa: &isa::ISA) -> String {
    let mut grammar = String::new();

    for e in isa.enums.iter() {
        let mut values: Vec<_> = e
            .values
            .iter()
            .filter(|v| !v.display.is_empty() && v.display != ".____")
            .collect();

        // From the pest docs:
        // The choice operator, written as a vertical line |, is ordered. The PEG
        // expression first | second means "try first; but if it fails, try second instead".
        //
        // We need to sort our enum to be able to parse eg th1.xxxx and t1.xxxx
        values.sort_by(|a, b| b.display.cmp(a.display));

        let rule_name = to_upper_camel_case(e.name.trim_start_matches('#'), true);

        let value_names: Vec<_> = values
            .iter()
            .map(|i| {
                to_upper_camel_case(i.name.unwrap_or(i.display), false)
                    .as_str()
                    .to_string()
            })
            .collect();

        grammar.push_str(&format!(
            "{} = {{ {} }}\n",
            rule_name,
            value_names.join(" | ")
        ));

        for value in &values {
            let variant_name = to_upper_camel_case(value.name.unwrap_or(value.display), false);
            grammar.push_str(&format!(
                "    {} = {{ \"{}\" }}\n",
                variant_name, value.display
            ));
        }

        grammar.push('\n')
    }

    grammar
}

/// Generates PEG grammar rules for instructions
///
/// # Arguments
/// * `isa` - A reference to the `ISA` struct
///
/// # Returns
/// A string containing the PEG grammar rules for instructions
fn generate_peg_grammar_instructions(isa: &isa::ISA) -> String {
    let mut grammar = String::new();

    // Collect instructions that do not start with "#"
    let instructions: Vec<_> = isa
        .bitsets
        .iter()
        .filter(|bitset| !bitset.name.starts_with('#'))
        .collect();

    // Generate instruction names
    let instruction_names: Vec<_> = instructions
        .iter()
        .map(|i| format!("Opc{}", to_upper_camel_case(i.name, true)))
        .collect();

    // Join instruction names and append to grammar
    grammar.push_str(&format!(
        "instruction = _{{ {} }}\n",
        instruction_names.join(" | ")
    ));

    for instruction in &instructions {
        let meta = isa.collect_meta(instruction.name);
        let r#type = meta.get("type").unwrap_or(&"");
        let opcode = format!("Opc{}", to_upper_camel_case(instruction.name, true));

        // Prepare rule parts
        let mut rule_parts = Vec::new();
        rule_parts.push(format!("\"{}\"", instruction.name));

        let template_key = format!("INSTR_{}", r#type.to_ascii_uppercase());
        let flags = isa
            .templates
            .get_by_key(&template_key.as_str())
            .map_or("", |template| template.display.trim());

        // Process flags
        // Convert the XML string to a vec and filter out not wanted NAME.
        // e.g.: {NAME}{DST_FULL}{SAT}{COND}{SKPHP}{TYPE}{PMODE}{THREAD}{RMODE} to
        // ["Dst_full", "Sat", "Cond", "Skphp", "Type", "Pmode", "Thread", "Rounding"]
        flags
            .split(&['{', '}'])
            .filter(|part| !part.trim().is_empty() && *part != "NAME")
            .for_each(|part| {
                let part = if part == "RMODE" { "Rounding" } else { part };
                rule_parts.push(format!("{}?", to_upper_camel_case(part, false)));
            });

        let has_dest = meta
            .get("has_dest")
            .unwrap_or(&"false")
            .parse::<bool>()
            .unwrap_or(false);

        let rule_part = match (has_dest, *r#type) {
            (true, "load_store") => "(Dest | DstMemAddr) ~ \",\"",
            (true, _) => "Dest ~ \",\"",
            (false, _) => "DestVoid ~ \",\"",
        };

        rule_parts.push(rule_part.to_string());

        if *r#type == "tex" {
            rule_parts.push("TexSrc ~ \",\"".to_string());
        }

        let possible_srcs = if *r#type == "cf" { 2 } else { 3 };
        let valid_srcs: Vec<_> = meta
            .get("valid_srcs")
            .unwrap_or(&"")
            .split('|')
            .filter_map(|s| s.parse::<usize>().ok())
            .collect();

        for i in 0..possible_srcs {
            if valid_srcs.contains(&i) {
                rule_parts.push("Src".to_string());
            } else {
                rule_parts.push("SrcVoid".to_string());
            }
            if i + 1 < possible_srcs {
                rule_parts.push("\",\"".to_string());
            }
        }

        if *r#type == "cf" {
            rule_parts.push("\",\"".to_string());
            rule_parts.push("Target".to_string());
        }

        grammar.push_str(&format!(
            "    {} = {{ {} }}\n",
            opcode,
            rule_parts.join(" ~ ")
        ));
    }

    grammar
}

/// Combines the PEG grammar rules for enums and instructions
///
/// # Arguments
/// * `isa` - A reference to the `ISA` struct
///
/// # Returns
/// A string containing the complete PEG grammar
fn generate_peg_grammar(isa: &isa::ISA) -> String {
    let mut grammar = String::new();

    grammar.push_str(&generate_peg_grammar_enums(isa));
    grammar.push_str(&generate_peg_grammar_instructions(isa));
    grammar.push_str("instructions = _{ SOI ~ (instruction ~ NEWLINE?)* ~ EOI }");

    grammar
}

/// Converts a string to UpperCamelCase
///
/// # Arguments
/// * `s` - The input string
/// * `rep_underscore` - Whether to replace underscores with spaces
///
/// # Returns
/// The converted string in UpperCamelCase
fn to_upper_camel_case(s: &str, rep_underscore: bool) -> String {
    // remove unwanted characters
    let mut s = s.replace(['.', '[', ']'], "");

    // optionally replace underscores with spaces
    if rep_underscore {
        s = s.replace('_', " ");
    }

    // capitalize the first letter of each word and join them
    s.split_whitespace()
        .map(|word| {
            let mut c = word.chars();
            match c.next() {
                Some(first) => {
                    first.to_uppercase().collect::<String>() + c.as_str().to_lowercase().as_str()
                }
                None => String::new(),
            }
        })
        .collect()
}

/// Procedural macro to derive the ISA parser
///
/// # Arguments
/// * `input` - The input token stream
///
/// # Returns
/// The output token stream
#[proc_macro_derive(IsaParser, attributes(isa, static_rules_file))]
pub fn derive_isaspec_parser(input: TokenStream) -> TokenStream {
    derive_parser(input)
}

#[cfg(test)]
mod lib {
    use super::*;
    use std::collections::HashMap;

    #[test]
    fn derive_ok() {
        let definition = "
            #[other_attr]
            #[isa = \"myfile.isa\"]
            #[static_rules_file = \"static_rules.pest\"]
            pub struct MyParser<'a, T>;
        ";
        let ast = syn::parse_str(definition).unwrap();
        let (isa, static_rules) = parse_derive(&ast);
        assert_eq!(isa, "myfile.isa");
        assert_eq!(static_rules, "static_rules.pest");
    }

    #[test]
    #[should_panic(expected = "Attribute must be a string")]
    fn derive_wrong_arg_isa() {
        let definition = "
            #[other_attr]
            #[isa = 1]
            #[static_rules_file = \"static_rules.pest\"]
            pub struct MyParser<'a, T>;
        ";
        let ast = syn::parse_str(definition).unwrap();
        parse_derive(&ast);
    }

    #[test]
    #[should_panic(expected = "Attribute must be a string")]
    fn derive_wrong_arg_static_rules_file() {
        let definition = "
            #[other_attr]
            #[isa = \"test.xml\"]
            #[static_rules_file = 1]
            pub struct MyParser<'a, T>;
        ";
        let ast = syn::parse_str(definition).unwrap();
        parse_derive(&ast);
    }

    #[test]
    #[should_panic(
        expected = "An ISA file needs to be provided with the #[isa = \"PATH\"] attribute"
    )]
    fn derive_no_isa() {
        let definition = "
            #[other_attr]
            pub struct MyParser<'a, T>;
        ";
        let ast = syn::parse_str(definition).unwrap();
        parse_derive(&ast);
    }

    #[test]
    fn test_to_upper_camel_case() {
        assert_eq!(to_upper_camel_case("test_string", true), "TestString");
        assert_eq!(to_upper_camel_case("test_string", false), "Test_string");
        assert_eq!(to_upper_camel_case("[Test]_String", true), "TestString");
        assert_eq!(to_upper_camel_case("[Test]_String", false), "Test_string");
        assert_eq!(
            to_upper_camel_case("multiple_words_string", true),
            "MultipleWordsString"
        );
    }

    fn mock_isa() -> isa::ISA<'static> {
        let mut bitsets = isa::IndexedMap::new();
        let mut enums = isa::IndexedMap::new();
        let mut templates = isa::IndexedMap::new();

        // Add mock data for bitsets, enums, and templates
        // Example for bitsets
        bitsets.insert(
            "bitset1",
            isa::Bitset {
                name: "bitset1",
                extends: None,
                meta: Some(HashMap::from([
                    ("type", "alu"),
                    ("has_dest", "true"),
                    ("valid_srcs", "0"),
                ])),
            },
        );

        // Example for enums
        enums.insert(
            "enum1",
            isa::BitSetEnum {
                name: "enum1",
                values: vec![
                    isa::BitSetEnumValue {
                        display: "val1",
                        name: Some("val1_name"),
                        value: "0",
                    },
                    isa::BitSetEnumValue {
                        display: "val2",
                        name: Some("val2_name"),
                        value: "1",
                    },
                ],
            },
        );

        // Example for templates
        templates.insert(
            "INSTR_ALU",
            isa::BitsetTemplate {
                name: "INSTR_ALU",
                display: "{DST_FULL}{SAT}{COND}",
            },
        );

        isa::ISA {
            bitsets,
            enums,
            templates,
        }
    }

    #[test]
    fn test_generate_peg_grammar_enums() {
        let isa = mock_isa();
        let grammar = generate_peg_grammar_enums(&isa);
        assert!(grammar.contains("Enum1 = { Val2 | Val1 }"));
        assert!(grammar.contains("Val1 = { \"val1\" }"));
        assert!(grammar.contains("Val2 = { \"val2\" }"));
    }

    #[test]
    fn test_generate_peg_grammar_instructions() {
        let isa = mock_isa();
        let grammar = generate_peg_grammar_instructions(&isa);
        assert!(grammar.contains("instructions = _{ OpcBitset1 }"));
        assert!(grammar.contains("OpcBitset1 = { \"bitset1\" ~ Dst_full? ~ Sat? ~ Cond? ~ Dest ~ \",\" ~ Src ~ \",\" ~ SrcVoid ~ \",\" ~ SrcVoid }"));
    }

    #[test]
    fn test_generate_peg_grammar() {
        let isa = mock_isa();
        let grammar = generate_peg_grammar(&isa);
        assert!(grammar.contains("Enum1 = { Val2 | Val1 }"));
        assert!(grammar.contains("instructions = _{ OpcBitset1 }"));
        assert!(grammar.contains("OpcBitset1 = { \"bitset1\" ~ Dst_full? ~ Sat? ~ Cond? ~ Dest ~ \",\" ~ Src ~ \",\" ~ SrcVoid ~ \",\" ~ SrcVoid }"));
    }
}
