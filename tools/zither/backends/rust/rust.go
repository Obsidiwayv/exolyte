// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package rust

import (
	"embed"
	"fmt"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"text/template"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
	"go.fuchsia.dev/fuchsia/zircon/tools/zither"
)

//go:embed templates/*
var templates embed.FS

// Generator provides go data layout bindings.
type Generator struct {
	fidlgen.Generator
}

func NewGenerator(formatter fidlgen.Formatter) *Generator {
	gen := fidlgen.NewGenerator("RustTemplates", templates, formatter, template.FuncMap{
		"LowerCaseWithUnderscores": LowerCaseWithUnderscores,
		"UpperCaseWithUnderscores": zither.UpperCaseWithUnderscores,
		"UpperCamelCase":           zither.UpperCamelCase,
		"ScalarTypeName":           ScalarTypeName,
		"Imports":                  Imports,
		"ConstType":                ConstType,
		"ConstValue":               ConstValue,
		"BitsAttributes":           BitsAttributes,
		"EnumAttributes":           EnumAttributes,
		"U64EnumAttributes":        U64EnumAttributes,
		"StructAttributes":         StructAttributes,
		"OverlayAttributes":        OverlayAttributes,
		"DescribeType": func(desc zither.TypeDescriptor) string {
			return DescribeType(desc, CaseStyleRust)
		},
	})
	return &Generator{*gen}
}

func (gen Generator) DeclOrder() zither.DeclOrder { return zither.SourceDeclOrder }

// The basic, derived traits that unconditionally figure into each declaration.
var defaultTraits = []string{"Copy", "Clone"}

// Some additional traits that require more computation to determine their
// support.
type traits struct {
	// Whether std::fmt::Debug is supported.
	debug bool

	// Whether std::cmp::Eq and std::cmp::PartialEq are supported.
	eq bool

	// Whether zerocopy::AsBytes is supported (effectively whether instances
	// are convertible to byte slices).
	//
	// https://docs.rs/zerocopy/latest/zerocopy/trait.AsBytes.html
	asBytes bool

	// Whether zerocopy::FromBytes is supported (effectively whether each
	// byte slice (of the same size and alignment) is uniquely converible to an
	// instance.
	//
	// https://docs.rs/zerocopy/latest/zerocopy/trait.FromBytes.html
	fromBytes bool
}

func (t traits) supported() []string {
	var supported []string
	if t.debug {
		supported = append(supported, "Debug")
	}
	if t.eq {
		supported = append(supported, "Eq", "PartialEq")
	}
	if t.asBytes {
		supported = append(supported, "AsBytes")
	}
	if t.fromBytes {
		supported = append(supported, "FromZeroes", "FromBytes")
	}
	return supported
}

// Additional traits by FIDL element name, this map is built up on each call to
// DeclCallback() within zither.Summarize().
var extraTraits = map[string]*traits{}

func (gen Generator) DeclCallback(decl zither.Decl) {
	name := decl.GetName().String()
	if _, ok := extraTraits[name]; ok {
		return // Already processed
	}
	t := new(traits)
	switch decl.(type) {
	case *zither.Enum:
		t.debug = true
		t.eq = true
		t.asBytes = true
	case *zither.Bits, *zither.Handle:
		t.debug = true
		t.eq = true
		t.asBytes = true
		t.fromBytes = true
	case *zither.Struct:
		t.debug = true
		t.eq = true
		s := decl.(*zither.Struct)
		t.asBytes = !s.HasPadding
		t.fromBytes = !s.HasPadding
		for _, m := range s.Members {
			mt := getExtraTraitsOfDependency(m.Type, name)
			t.debug = t.debug && mt.debug
			t.eq = t.eq && mt.eq
			t.asBytes = t.asBytes && mt.asBytes
			t.fromBytes = t.fromBytes && mt.fromBytes
		}
	case *zither.Overlay:
		o := decl.(*zither.Overlay)
		t.asBytes = true
		for _, m := range o.Variants {
			// AsBytes can only be derived if no variant has padding.
			t.asBytes = t.asBytes && //
				m.Type.Size == o.MaxVariantSize && //
				getExtraTraitsOfDependency(m.Type, name).asBytes
		}
	case *zither.Alias:
		a := decl.(*zither.Alias)
		t = getExtraTraitsOfDependency(a.Value, name)
	}
	extraTraits[name] = t
}

func getExtraTraitsOfDependency(dep zither.TypeDescriptor, dependent string) *traits {
	t := new(traits)
	switch dep.Kind {
	case zither.TypeKindBool:
		t.debug = true
		t.eq = true
		t.asBytes = true
	case zither.TypeKindEnum, zither.TypeKindBits, zither.TypeKindStruct,
		zither.TypeKindOverlay, zither.TypeKindAlias:
		var ok bool
		t, ok = extraTraits[dep.Type]
		if !ok {
			panic(fmt.Sprintf("Processing %s: unprocessed dependency: %s", dependent, dep.Type))
		}
	case zither.TypeKindArray:
		t = getExtraTraitsOfDependency(*dep.ElementType, dependent)
	default:
		t.debug = true
		t.eq = true
		if !dep.Kind.IsPointerLike() {
			t.asBytes = true
			t.fromBytes = true
		}
	}
	return t
}

func (gen *Generator) Generate(summary zither.LibrarySummary, outputDir string) ([]string, error) {
	lib := summary.Library
	rootData := crateRootData{Library: lib}
	crateParts := append([]string{"fidl", "data"}, lib.Parts()...)
	outputDir = filepath.Join(outputDir, strings.Join(crateParts, "-"), "src")

	var outputs []string
	for _, summary := range summary.Files {
		// Hyphens are illegal in module names.
		moduleName := strings.ReplaceAll(summary.Name(), "-", "_")
		rootData.Modules = append(rootData.Modules, moduleName)
		output := filepath.Join(outputDir, moduleName+".rs")
		if err := gen.GenerateFile(output, "GenerateRustFile", summary); err != nil {
			return nil, err
		}
		outputs = append(outputs, output)
	}

	crateRoot := filepath.Join(outputDir, "lib.rs")
	sort.Strings(rootData.Modules) // Default `Modules` order may be nondeterministic.
	if err := gen.GenerateFile(crateRoot, "GenerateRustCrateRoot", rootData); err != nil {
		return nil, err
	}
	outputs = append(outputs, crateRoot)
	return outputs, nil
}

// crateRootData is data for the "GenerateRustCrateRoot" template.
type crateRootData struct {
	Library fidlgen.LibraryName
	Modules []string
}

//
// Template functions.
//

func LowerCaseWithUnderscores(el zither.Element) string {
	// Be sure to escape keywords.
	reservedKeywords := map[string]struct{}{
		"type": {},
	}
	val := zither.LowerCaseWithUnderscores(el)
	if _, ok := reservedKeywords[val]; ok {
		val = "r#" + val
	}
	return val
}

func ScalarTypeName(typ fidlgen.PrimitiveSubtype) string {
	switch typ {
	case fidlgen.Bool:
		return "bool"
	case fidlgen.Int8:
		return "i8"
	case fidlgen.Int16:
		return "i16"
	case fidlgen.Int32:
		return "i32"
	case fidlgen.Int64:
		return "i64"
	case fidlgen.Uint8, fidlgen.ZxExperimentalUchar:
		return "u8"
	case fidlgen.Uint16:
		return "u16"
	case fidlgen.Uint32:
		return "u32"
	case fidlgen.Uint64:
		return "u64"
	case fidlgen.ZxExperimentalUsize64, fidlgen.ZxExperimentalUintptr64:
		return "usize"
	default:
		panic(fmt.Sprintf("%s unknown FIDL primitive type: %s", typ))
	}
}

func Imports(summary zither.FileSummary) []string {
	var imports []string
	for _, kind := range summary.TypeKinds() {
		switch kind {
		case zither.TypeKindBits:
			imports = append(imports, "bitflags::bitflags")
		}
	}

	asBytes := false
	fromBytes := false
	for _, decl := range summary.Decls {
		t := extraTraits[decl.Name().String()]
		asBytes = asBytes || t.asBytes
		fromBytes = fromBytes || t.fromBytes
		if asBytes && fromBytes {
			break
		}
	}
	if asBytes || fromBytes {
		var zerocopyImports []string
		if asBytes {
			zerocopyImports = append(zerocopyImports, "AsBytes")
		}
		if fromBytes {
			zerocopyImports = append(zerocopyImports, "FromZeroes", "FromBytes")
		}
		imports = append(imports, fmt.Sprintf("zerocopy::{%s}", strings.Join(zerocopyImports, ", ")))
	}
	return imports
}

func ConstType(c zither.Const) string {
	switch c.Kind {
	case zither.TypeKindBool, zither.TypeKindInteger, zither.TypeKindSize:
		return ScalarTypeName(fidlgen.PrimitiveSubtype(c.Type))
	case zither.TypeKindString:
		return "&str"
	case zither.TypeKindEnum, zither.TypeKindBits:
		return zither.UpperCamelCase(c.Element.Decl)
	default:
		panic(fmt.Sprintf("%s has unknown constant kind: %s", c.Name, c.Type))
	}
}

func ConstValue(c zither.Const) string {
	if c.Element != nil {
		switch c.Kind {
		case zither.TypeKindEnum:
			return zither.UpperCamelCase(c.Element.Decl) + "::" + zither.UpperCamelCase(c.Element.Member)
		case zither.TypeKindBits:
			if c.Element.Member != nil {
				return zither.UpperCamelCase(c.Element.Decl) + "::" + zither.UpperCaseWithUnderscores(c.Element.Member)
			}
			val, err := strconv.Atoi(c.Value)
			if err != nil {
				panic(fmt.Sprintf("%s has malformed integral value: %s", c.Name, err))
			}
			return fmt.Sprintf("%s::from_bits_truncate(%#b)", zither.UpperCamelCase(c.Element.Decl), val)
		default:
			return zither.UpperCaseWithUnderscores(c.Element.Decl)
		}
	}

	switch c.Kind {
	case zither.TypeKindString:
		return fmt.Sprintf("%q", c.Value)
	case zither.TypeKindBool, zither.TypeKindInteger, zither.TypeKindSize:
		return c.Value
	case zither.TypeKindEnum, zither.TypeKindBits:
		// Enum and bits constants should have been handled above.
		panic(fmt.Sprintf("enum and bits constants must be given by an `Element` value: %#v", c))
	default:
		panic(fmt.Sprintf("%s has unknown constant kind: %s", c.Name, c.Type))
	}
}

func layoutAttributes(decl zither.Decl) []string {
	repr := "#[repr(C)]"
	if e, ok := decl.(*zither.Enum); ok {
		repr = fmt.Sprintf("#[repr(%s)]", ScalarTypeName(e.Subtype))
	}

	t := extraTraits[decl.GetName().String()]
	supported := append(defaultTraits, t.supported()...)
	sort.Strings(supported)

	return []string{
		repr,
		fmt.Sprintf("#[derive(%s)]", strings.Join(supported, ", ")),
	}
}

func EnumAttributes(e zither.Enum) []string { return layoutAttributes(&e) }

func U64EnumAttributes() []string {
	supported := append(defaultTraits, "AsBytes", "Debug", "Eq", "PartialEq")
	sort.Strings(supported)
	return []string{
		"#[repr(u64)]",
		fmt.Sprintf("#[derive(%s)]", strings.Join(supported, ", ")),
	}
}

func BitsAttributes() []string {
	// The default traits are already implicitly derived via the bitflags!
	// macro.
	return []string{
		"#[repr(C)]",
		"#[derive(AsBytes, FromZeroes, FromBytes)]",
	}
}

func StructAttributes(s zither.Struct) []string { return layoutAttributes(&s) }

func OverlayAttributes(o zither.Overlay) []string { return layoutAttributes(&o) }

// CaseStyle represents a style of casing Rust type names.
type CaseStyle int

const (
	// CaseStyleRust represents official rust style.
	CaseStyleRust CaseStyle = iota

	// CaseStyleSyscall gives a C-style spelling for certain types, for
	// suggestive use in the Rust FFI syscall wrapper definitions.
	CaseStyleSyscall
)

func DescribeType(desc zither.TypeDescriptor, style CaseStyle) string {
	var casify func(string) string
	switch style {
	case CaseStyleRust:
		casify = fidlgen.ToUpperCamelCase
	case CaseStyleSyscall:
		casify = func(name string) string {
			typ := fidlgen.ToSnakeCase(name)
			switch desc.Kind {
			case zither.TypeKindAlias, zither.TypeKindStruct, zither.TypeKindEnum,
				zither.TypeKindBits, zither.TypeKindHandle:
				return "zx_" + typ + "_t"
			default:
				return typ
			}
		}
	}

	switch desc.Kind {
	case zither.TypeKindBool, zither.TypeKindInteger, zither.TypeKindSize:
		return ScalarTypeName(fidlgen.PrimitiveSubtype(desc.Type))
	case zither.TypeKindEnum, zither.TypeKindBits, zither.TypeKindStruct, zither.TypeKindOverlay:
		layout, _ := fidlgen.MustReadName(desc.Type).SplitMember()
		return casify(layout.DeclarationName())
	case zither.TypeKindAlias, zither.TypeKindHandle:
		// TODO(https://fxbug.dev/42057022): This assumes that the alias/handle was defined
		// within the same package. That's true now, but this would need to be
		// re-evaluated if/when zither supports library dependencies and the IR
		// preserves imported alias names.
		return casify(fidlgen.MustReadName(desc.Type).DeclarationName())
	case zither.TypeKindArray:
		return fmt.Sprintf("[%s; %d]", DescribeType(*desc.ElementType, style), *desc.ElementCount)
	case zither.TypeKindStringArray:
		return fmt.Sprintf("[u8; %d]", *desc.ElementCount)
	case zither.TypeKindPointer, zither.TypeKindVoidPointer:
		mutability := "const"
		if desc.ElementType.Mutable {
			mutability = "mut"
		}
		return fmt.Sprintf("*%s %s", mutability, DescribeType(*desc.ElementType, style))
	default:
		panic(fmt.Sprintf("unsupported type kind: %v", desc.Kind))
	}
}
