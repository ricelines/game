"""The owner/parameter guard must distinguish declarations from incidental text."""

from tools.check_architecture import header_parameter_counts, mixed_state_structs


def test_parameter_limit_counts_nested_types_and_callback_as_single_parameters():
    source = """
    void oversized(std::array<int, 2>, std::map<int, std::vector<int>>,
                   void (*callback)(int, double), int, int, int, int, int, int, int, int);
    void boundary(int, int, int, int, int, int, int, int, int, int);
    """
    counts = {name: count for name, count, _ in header_parameter_counts(source)}
    assert counts == {"oversized": 11, "boundary": 10}


def test_parameter_guard_ignores_comments_strings_and_default_value_commas():
    source = """
    // void fake(int, int, int, int, int, int, int, int, int, int, int);
    const char *text = R"note(void fake(int, int, int);)note";
    void actual(std::array<int, 2> values = {1, 2}, int other = call(1, 2));
    """
    assert [(name, count) for name, count, _ in header_parameter_counts(source)] == [("actual", 2)]


def test_parameter_guard_checks_inline_header_definitions():
    source = (
        "int inline_function(int, int, int, int, int, int, int, int, int, int, int) { return 0; }"
    )
    assert header_parameter_counts(source) == [("inline_function", 11, 1)]


def test_state_guard_rejects_fields_across_owners_including_object_owner():
    assert mixed_state_structs("struct Bad { react::State r; perm::State *p; };") == ["Bad"]
    assert mixed_state_structs(
        "struct Bad { const amber::ObjectState &objects; amber::catalog::State catalog; };"
    ) == ["Bad"]


def test_state_guard_allows_one_owner_and_method_arguments():
    assert (
        mixed_state_structs(
            """struct Fine {
            react::State one, two;
            void call(perm::State &, catalog::State &);
        };"""
        )
        == []
    )
    assert (
        mixed_state_structs(
            """struct Fine {
            void call(perm::State &, catalog::State &) {}
            react::State one;
            react::State two;
        };"""
        )
        == []
    )


def test_state_guard_sees_members_after_inline_methods():
    assert mixed_state_structs(
        "struct Bad { void method() {} react::State first; net::State second; };"
    ) == ["Bad"]


def test_parameter_guard_does_not_mistake_inline_calls_for_declarations():
    source = "void wrapper() { target::call(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11); }"
    assert header_parameter_counts(source) == [("wrapper", 0, 1)]


def test_parameter_exceptions_are_limited_to_snapshot_composition(tmp_path, monkeypatch):
    from tools import check_architecture as architecture

    directory = tmp_path / "step"
    directory.mkdir()
    names = ["encode_checkpoint", "decode_checkpoint", "save_snapshot", "flush_gaps", "other"]
    (directory / "step.hpp").write_text(
        "\n".join(f"void {name}({', '.join(['int'] * 11)});" for name in names)
    )
    monkeypatch.setattr(architecture, "SERVER", tmp_path)
    errors = [error for error in architecture.check() if "declares 11 parameters" in error]
    assert len(errors) == 1 and "other declares 11 parameters" in errors[0]


def test_store_header_rejects_foreign_declarations_and_json(tmp_path, monkeypatch):
    from tools import check_architecture as architecture

    directory = tmp_path / "store"
    directory.mkdir()
    (directory / "store.hpp").write_text(
        "#include <nlohmann/json.hpp>\nnamespace amber::catalog { struct Asset {}; }\n"
    )
    monkeypatch.setattr(architecture, "SERVER", tmp_path)
    errors = architecture.check()
    assert any("another owner's declarations in the store header" in error for error in errors)
    assert any("JSON implementation in the store header" in error for error in errors)
