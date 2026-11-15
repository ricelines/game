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
