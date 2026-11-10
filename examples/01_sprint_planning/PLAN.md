---
project: Mobile App v2.0
sprint: Sprint 14
team: Mobile Engineering
---

## User Stories

- [~] As a user, I can reset my password #p0 ~4h @developer !2025-01-20
  - [x] Design reset flow
  - [~] Implement email service
  - [ ] Build reset form
  - ( ) Unit tests pass
  - ( ) QA sign-off
- [ ] As a user, I can enable 2FA #p1 ~8h @tester !2025-01-22
  - [ ] Research TOTP libraries
  - [ ] Design 2FA setup flow
  - [ ] Implement QR code generation
  - [ ] Build verification screen

## Technical Debt

- [ ] Refactor authentication module #p2 ~4h @developer
  - Current code is hard to maintain
  - Need to split into smaller services
- [ ] Update deprecated dependencies #p3 ~2h @tester

## Bugs

- [!] App crashes on Android 13 #p0 @developer
  - blocked-by: ^android-sdk-update
  - Affects 15% of users
  - Related to camera permissions
- [~] Slow loading on first launch #p1 ~4h @developer
  - [x] Profile the app
  - [~] Optimize bundle size
  - [ ] Implement lazy loading
