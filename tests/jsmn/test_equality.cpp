#undef NDEBUG
#include "Jsmn/Object.hpp"
#include <cassert>
#include <iostream>

void test_null_equality() {
    auto json_null1 = Jsmn::Object::parse_json(R"JSON({ "x": null })JSON");
    auto json_null2 = Jsmn::Object::parse_json(R"JSON({ "x": null })JSON");
    assert(json_null1 == json_null2);

    auto json_non_null = Jsmn::Object::parse_json(R"JSON({ "x": 1 })JSON");
    assert(json_null1 != json_non_null);
}

void test_boolean_equality() {
    auto json_true1 = Jsmn::Object::parse_json(R"JSON({ "x": true })JSON");
    auto json_true2 = Jsmn::Object::parse_json(R"JSON({ "x": true })JSON");
    assert(json_true1 == json_true2);

    auto json_false1 = Jsmn::Object::parse_json(R"JSON({ "x": false })JSON");
    auto json_false2 = Jsmn::Object::parse_json(R"JSON({ "x": false })JSON");
    assert(json_false1 == json_false2);

    assert(json_true1 != json_false1);
}

void test_string_equality() {
    auto json_str1 = Jsmn::Object::parse_json(R"JSON({ "x": "hello" })JSON");
    auto json_str2 = Jsmn::Object::parse_json(R"JSON({ "x": "hello" })JSON");
    assert(json_str1 == json_str2);

    auto json_str3 = Jsmn::Object::parse_json(R"JSON({ "x": "world" })JSON");
    assert(json_str1 != json_str3);
}

void test_number_equality() {
    auto json_num1 = Jsmn::Object::parse_json(R"JSON({ "x": 42 })JSON");
    auto json_num2 = Jsmn::Object::parse_json(R"JSON({ "x": 42 })JSON");
    assert(json_num1 == json_num2);

    auto json_num3 = Jsmn::Object::parse_json(R"JSON({ "x": 3.14 })JSON");
    assert(json_num1 != json_num3);
}

void test_object_equality() {
    auto json_obj1 = Jsmn::Object::parse_json(R"JSON({ "a": 1, "b": 2 })JSON");
    auto json_obj2 = Jsmn::Object::parse_json(R"JSON({ "b": 2, "a": 1 })JSON");
    assert(json_obj1 == json_obj2);

    auto json_obj3 = Jsmn::Object::parse_json(R"JSON({ "a": 1, "b": 3 })JSON");
    assert(json_obj1 != json_obj3);
}

void test_array_equality() {
    auto json_arr1 = Jsmn::Object::parse_json(R"JSON([1, 2, 3])JSON");
    auto json_arr2 = Jsmn::Object::parse_json(R"JSON([1, 2, 3])JSON");
    assert(json_arr1 == json_arr2);

    auto json_arr3 = Jsmn::Object::parse_json(R"JSON([3, 2, 1])JSON");
    assert(json_arr1 != json_arr3);
}

void test_nested_structure_equality() {
    auto json_nested1 = Jsmn::Object::parse_json(R"JSON({ "x": [ { "a": 1 }, { "b": 2 } ] })JSON");
    auto json_nested2 = Jsmn::Object::parse_json(R"JSON({ "x": [ { "a": 1 }, { "b": 2 } ] })JSON");
    assert(json_nested1 == json_nested2);

    auto json_nested3 = Jsmn::Object::parse_json(R"JSON({ "x": [ { "b": 2 }, { "a": 1 } ] })JSON");
    assert(json_nested1 != json_nested3);
}

int main() {
    test_null_equality();
    test_boolean_equality();
    test_string_equality();
    test_number_equality();
    test_object_equality();
    test_array_equality();
    test_nested_structure_equality();
    return 0;
}
