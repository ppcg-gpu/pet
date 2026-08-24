/* A class template with specialisations, which every unit sees alike.
 *
 * Each specialisation is a record of its own and all of them are called
 * after the template, so counting records by the bare name makes these
 * four look like four entities of one name -- which is what a link that
 * failed to make two structs one also looks like.  ggml has a
 * type_to_gguf_type with twelve of them and a type_conversion_table
 * with four, and the report used to say a whole engine had records
 * split when nothing was wrong with it.
 */
#ifndef PET_TESTS_SPLIT_TEMPLATE_H
#define PET_TESTS_SPLIT_TEMPLATE_H

template <typename T>
struct width_of;

template <>
struct width_of<char> { static const int bits = 8; };

template <>
struct width_of<short> { static const int bits = 16; };

template <>
struct width_of<int> { static const int bits = 32; };

template <>
struct width_of<long> { static const int bits = 64; };

#endif
