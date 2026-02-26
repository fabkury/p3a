# Separate Domain Logic From Transport

## Goal
Keep Makapix and channel logic independent of MQTT/HTTP so it can be reused or tested without a network stack.

## Current Cues In The Codebase
- Makapix module mixes API calls, provisioning, MQTT, and state tracking in one component.
- Channel switching and artwork selection involve direct calls into Makapix logic.

## First-Principles Rationale
Domain logic should not depend on transport details. This makes it:
- Easier to test
- Easier to replace transports (e.g., alternative servers)
- Easier to reason about failures

## Concrete Steps
1. Extract a `makapix_domain` layer that defines commands and responses.
2. Implement adapters for HTTP and MQTT in a `makapix_transport` layer.
3. The domain layer consumes abstract responses and emits events (or uses callbacks).

## Risks And Mitigations
- Risk: higher initial complexity.
- Mitigation: start by isolating one slice (provisioning or channel refresh).

## Success Criteria
- Domain logic can be compiled without MQTT or HTTP dependencies.
- Transport can be swapped with minimal changes.
