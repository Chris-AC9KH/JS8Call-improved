/**
 * @file ActivitySettingsKeys.h
 * @brief The settings keys that bind a configuration to its activity.
 */
#pragma once

/**
 * @namespace ActivitySettings
 * @brief The per-configuration settings keys of the activity store.
 *
 * Declared once and shared, because MultiSettings writes the clone
 * marker that ActivityStorageController reads: were the two spellings to
 * drift apart, a clone would silently keep its source's id and the two
 * configurations would then share - and clear - each other's history.
 */
namespace ActivitySettings {
/// The key a configuration stores its activity store id under.
inline constexpr char ACTIVITY_DB_ID_KEY[] = "ActivityDbId";
/// The marker that replaces that id in a clone, naming its source.
inline constexpr char ACTIVITY_DB_CLONE_FROM_KEY[] = "ActivityDbCloneFrom";
} // namespace ActivitySettings
