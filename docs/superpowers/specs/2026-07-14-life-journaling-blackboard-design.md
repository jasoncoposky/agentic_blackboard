# Design Spec: Life Journaling & Wellness Blackboard Substrate
**Date**: 2026-07-14  
**Status**: APPROVED (Approach A)  

---

## 1. Background & Objectives
The Agentic Sovereign Orchestration Substrate (ASOS) Blackboard was originally optimized for software engineering workflows (anchored on SWEBOK Knowledge Areas and SDLC entities). However, to pivot the substrate to serve as a holistic life tracking and journaling engine, we must expand the underlying schema.

The objective is to introduce wellness metrics, physical and mental activities, learning/education logs, and geographical/physical coordinates into the C++ Zero-Copy BSON graph model, while keeping the database backward-compatible with software engineering entries.

---

## 2. Proposed Changes

### 2.1. Semantic Graph Relationships (`rel` Namespace)
To support rich journaling connections, we introduce two new edge labels:
* **`MENTIONS`**: Directed edge from a journal entry (`CpbEntry`) to a person's profile (`IdentityNode`).
* **`OCCURRED_AT`**: Directed edge from a journal entry (`CpbEntry`) to a tracking coordinate (`SpatialAnchorNode`).

### 2.2. Extended Taxonomy (`KnowledgeArea` Enum)
We add new categorization codes to class `KnowledgeArea`:
* `HEALTH_WELLNESS = 20` (wellness, sleep, workouts)
* `SOCIAL_RELATIONSHIPS = 21` (friends, family, social logs)
* `PERSONAL_REFLECTIONS = 22` (daily thoughts, feelings, gratitude)
* `LEISURE_CREATIVITY = 23` (hobbies, gaming, arts, leisure time)
* `DAILY_ROUTINE = 24` (habits, logs of sleep, diet)
* `EDUCATION_LEARNING = 25` (academic pursuits, tutorial notes, research, reading books)

### 2.3. Extended Data Payloads (`Wellness` and `Education` Structures)
We introduce two optional, nested structures inside the `CpbEntry` struct:

```cpp
struct Wellness {
    // Mood & Energy
    double mood_sentiment = 0.0;   // -1.0 (negative) to 1.0 (positive)
    double energy_level = 0.0;     // 1.0 (low) to 10.0 (high)
    double sleep_hours = 0.0;      // Sleep duration

    // Activity Tracking
    double active_minutes = 0.0;   // Workout duration
    int64_t step_count = 0;        // Physical movement steps
    std::string activity_type;     // e.g., "running", "walking", "meditating"
};

struct Education {
    std::string institution_platform;  // e.g., "Coursera", "MIT", "Self-Directed"
    std::string resource_type;         // e.g., "book", "lecture", "paper", "lab"
    double progress_percent = 0.0;     // Progress (0.0 to 100.0)
    double focus_duration_minutes = 0.0; // Study session duration
    std::string credential_uuid;       // Optional link to certification node
};
```

---

## 3. Serialization and Backward Compatibility
To avoid bloating the database for standard software engineering entries, `wellness` and `education` will be stored as `std::optional` fields inside `CpbEntry`:

1. **Serialization**: If `wellness.has_value()` is true, a nested BSON object with the key `"wellness"` is written to the buffer. If `education.has_value()` is true, a nested BSON object with the key `"education"` is written. Otherwise, they are skipped.
2. **Deserialization**: The parser checks if `"wellness"` or `"education"` exists as an object type in the root BSON document. If found, it populates the respective field; otherwise, they are left as `std::nullopt`.

This guarantees 100% backward compatibility for all existing records.

---

## 4. Security & Multi-Tenancy Enforcement (l3kvg ACL Integration)
To support multi-tenancy and prevent key collisions or unauthorized access between different users, the Blackboard leverages the built-in, distributed `CredentialManager` of `l3kvg`:

1. **User UID Mapping**: Every root identity (e.g. `"identity:jasoncoposky"`) is mapped deterministically to a `uint32_t` UID (principal ID).
2. **Database Registration**: On startup or user registration, the Blackboard registers the identity with `L3KV`'s credentials and defines prefix-based ACL rules:
   * Grantees are allowed `READ` and `WRITE` permissions only on keys prefixed with their namespace (e.g. `n:{username:`).
3. **Database-Level Protection**:
   * All database gets, puts, and query traversals execute with the client's `principal_id`.
   * Cross-user reads or modifications trigger an automatic `Access Denied` exception at the database layer.

---

## 5. Verification Plan
1. **Compilation**: Compile the library and executables using `cmake`.
2. **Unit Testing**: Extend the verification tool `asos_verify`:
   - Verify that a journal entry containing a `Wellness` payload serializes, deserializes, and traverses edge lookups correctly.
   - Seed a root identity node, register its ACL credentials in the storage engine, and verify that reads/writes using its `principal_id` succeed for its own prefix and fail for another user's prefix.

