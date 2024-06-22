// Copyright © 2024 Igalia S.L.
// SPDX-License-Identifier: MIT

use roxmltree::Document;
use std::collections::HashMap;

/// A structure that holds a vector and a map to allow for efficient access by key or by index.
pub struct IndexedMap<K, V> {
    vec: Vec<V>,
    map: HashMap<K, usize>,
}

impl<K, V> IndexedMap<K, V>
where
    K: std::hash::Hash + Eq,
{
    /// Creates a new, empty `IndexedMap`.
    pub fn new() -> Self {
        IndexedMap {
            vec: Vec::new(),
            map: HashMap::new(),
        }
    }

    /// Inserts a key-value pair into the `IndexedMap`.
    pub fn insert(&mut self, key: K, value: V) {
        self.vec.push(value);
        let index = self.vec.len() - 1;
        self.map.insert(key, index);
    }

    /// Gets a reference to the value associated with the given key.
    pub fn get_by_key(&self, key: &K) -> Option<&V> {
        self.map.get(key).map(|&index| &self.vec[index])
    }

    /// Returns an iterator over the values in the `IndexedMap`.
    pub fn iter(&self) -> IndexedMapIter<K, V> {
        IndexedMapIter {
            indexed_map: self,
            index: 0,
        }
    }
}

/// An iterator over the values in an `IndexedMap`.
pub struct IndexedMapIter<'a, K, V> {
    indexed_map: &'a IndexedMap<K, V>,
    index: usize,
}

impl<'a, K, V> Iterator for IndexedMapIter<'a, K, V> {
    type Item = &'a V;

    fn next(&mut self) -> Option<Self::Item> {
        if self.index < self.indexed_map.vec.len() {
            let item = &self.indexed_map.vec[self.index];
            self.index += 1;
            Some(item)
        } else {
            None
        }
    }
}

/// A structure representing a bitset.
#[derive(Debug)]
pub struct Bitset<'a> {
    pub name: &'a str,
    pub extends: Option<&'a str>,
    pub meta: Option<HashMap<&'a str, &'a str>>,
}

/// A structure representing a value in a bitset enum.
#[derive(Debug)]
pub struct BitSetEnumValue<'a> {
    pub display: &'a str,
    pub name: Option<&'a str>,
    pub value: &'a str,
}

/// A structure representing a bitset enum.
#[derive(Debug)]
pub struct BitSetEnum<'a> {
    pub name: &'a str,
    pub values: Vec<BitSetEnumValue<'a>>,
}

/// A structure representing a bitset template.
#[derive(Debug)]
pub struct BitsetTemplate<'a> {
    pub name: &'a str,
    pub display: &'a str,
}

/// A structure representing an Instruction Set Architecture (ISA),
/// containing bitsets and enums.
pub struct ISA<'a> {
    pub bitsets: IndexedMap<&'a str, Bitset<'a>>,
    pub enums: IndexedMap<&'a str, BitSetEnum<'a>>,
    pub templates: IndexedMap<&'a str, BitsetTemplate<'a>>,
}

impl<'a> ISA<'a> {
    /// Creates a new `ISA` by loading bitsets and enums from a parsed XML document.
    pub fn new(doc: &'a Document) -> Self {
        let mut isa = ISA {
            bitsets: IndexedMap::new(),
            enums: IndexedMap::new(),
            templates: IndexedMap::new(),
        };

        isa.load_from_document(doc);
        isa
    }

    /// Collects metadata for a given bitset by walking the `extends` chain in reverse order.
    pub fn collect_meta(&self, name: &'a str) -> HashMap<&'a str, &'a str> {
        let mut meta = HashMap::new();
        let mut chain = Vec::new();
        let mut current = Some(name);

        // Gather the chain of bitsets
        while let Some(item) = current {
            if let Some(bitset) = self.bitsets.get_by_key(&item) {
                chain.push(bitset);
                current = bitset.extends;
            } else {
                current = None;
            }
        }

        // Collect metadata in reverse order
        for bitset in chain.into_iter().rev() {
            if let Some(m) = &bitset.meta {
                meta.extend(m.clone());
            }
        }

        meta
    }

    /// Loads bitsets and enums from a parsed XML document into the `ISA`.
    fn load_from_document(&mut self, doc: &'a Document) {
        doc.descendants()
            .filter(|node| node.is_element() && node.has_tag_name("template"))
            .for_each(|value| {
                let name = value.attribute("name").unwrap();
                let display = value.text().unwrap();

                self.templates
                    .insert(name, BitsetTemplate { name, display });
            });

        doc.descendants()
            .filter(|node| node.is_element() && node.has_tag_name("enum"))
            .for_each(|node| {
                let values = node
                    .children()
                    .filter(|node| node.is_element() && node.has_tag_name("value"))
                    .map(|value| {
                        let display = value.attribute("display").unwrap();
                        let name = value.attribute("name");
                        let value = value.attribute("val").unwrap();

                        BitSetEnumValue {
                            display,
                            name,
                            value,
                        }
                    })
                    .collect();

                let name = node.attribute("name").unwrap();

                self.enums.insert(name, BitSetEnum { name, values });
            });

        doc.descendants()
            .filter(|node| node.is_element() && node.has_tag_name("bitset"))
            .for_each(|node| {
                let name = node.attribute("name").unwrap();
                let extends = node.attribute("extends");
                let meta_nodes = node
                    .children()
                    .filter(|child| child.is_element() && child.has_tag_name("meta"));

                // We can have multiple <meta> tags, which we need to combine.
                let mut combined_meta: HashMap<&str, &str> = HashMap::new();

                meta_nodes.for_each(|m| {
                    m.attributes().for_each(|attr| {
                        combined_meta.insert(attr.name(), attr.value());
                    });
                });

                let meta = if combined_meta.is_empty() {
                    None
                } else {
                    Some(combined_meta)
                };

                self.bitsets.insert(
                    name,
                    Bitset {
                        name,
                        extends,
                        meta,
                    },
                );
            });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_indexed_map_insert_and_get_by_key() {
        let mut map = IndexedMap::new();
        map.insert("key1", 10);
        map.insert("key2", 20);

        assert_eq!(map.get_by_key(&"key1"), Some(&10));
        assert_eq!(map.get_by_key(&"key2"), Some(&20));
        assert_eq!(map.get_by_key(&"key3"), None);
    }

    #[test]
    fn test_indexed_map_iteration() {
        let mut map = IndexedMap::new();
        map.insert("key1", 10);
        map.insert("key2", 20);

        let values: Vec<&i32> = map.iter().collect();
        assert_eq!(values, vec![&10, &20]);
    }

    #[test]
    fn test_collect_meta() {
        let mut isa = ISA {
            bitsets: IndexedMap::new(),
            enums: IndexedMap::new(),
            templates: IndexedMap::new(),
        };
        isa.bitsets.insert(
            "bitset1",
            Bitset {
                name: "bitset1",
                extends: None,
                meta: Some(HashMap::from([("key1", "value1")])),
            },
        );
        isa.bitsets.insert(
            "bitset2",
            Bitset {
                name: "bitset2",
                extends: Some("bitset1"),
                meta: Some(HashMap::from([("key2", "value2")])),
            },
        );
        isa.bitsets.insert(
            "bitset3",
            Bitset {
                name: "bitset3",
                extends: Some("bitset2"),
                meta: Some(HashMap::from([("key3", "value3")])),
            },
        );

        let meta = isa.collect_meta("bitset3");
        assert_eq!(meta.get("key1"), Some(&"value1"));
        assert_eq!(meta.get("key2"), Some(&"value2"));
        assert_eq!(meta.get("key3"), Some(&"value3"));
    }

    #[test]
    fn test_load_from_document() {
        let xml_data = r#"
        <isa>
            <bitset name="bitset1">
                <meta key1="value1"/>
                <meta key2="value2"/>
            </bitset>
            <bitset name="bitset2" extends="bitset1"/>
            <enum name="enum1">
                <value display="val1" val="0"/>
                <value display="val2" val="1"/>
            </enum>
        </isa>
        "#;

        let doc = Document::parse(xml_data).unwrap();
        let isa = ISA::new(&doc);

        let bitset1 = isa.bitsets.get_by_key(&"bitset1").unwrap();
        assert_eq!(bitset1.name, "bitset1");
        assert_eq!(bitset1.meta.as_ref().unwrap().get("key1"), Some(&"value1"));
        assert_eq!(bitset1.meta.as_ref().unwrap().get("key2"), Some(&"value2"));

        let bitset2 = isa.bitsets.get_by_key(&"bitset2").unwrap();
        assert_eq!(bitset2.name, "bitset2");
        assert_eq!(bitset2.extends, Some("bitset1"));

        let enum1 = isa.enums.get_by_key(&"enum1").unwrap();
        assert_eq!(enum1.name, "enum1");
        assert_eq!(enum1.values.len(), 2);
        assert_eq!(enum1.values[0].display, "val1");
        assert_eq!(enum1.values[0].value, "0");
        assert_eq!(enum1.values[1].display, "val2");
        assert_eq!(enum1.values[1].value, "1");
    }
}
