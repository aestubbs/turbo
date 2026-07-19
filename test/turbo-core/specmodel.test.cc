#include <turbo/specmodel.h>

#include <gtest/gtest.h>

using namespace turbo;

namespace {

const char kDoc[] =
    "---\n"
    "title: Spec Workbench\n"
    "status: reviewed\n"
    "domain: ide-feature\n"
    "created: 2026-07-19\n"
    "updated: 2026-07-19\n"
    "id: workbench\n"
    "depends: [spec-a, spec-b]\n"
    "supersedes: old-spec\n"
    "---\n"
    "\n"
    "# Background\n"
    "\n"
    "Some background.\n"
    "\n"
    "# Objective\n"
    "\n"
    "# Decisions\n"
    "\n"
    "- **D1 (2026-07-19):** decided.\n"
    "- **Q8 — Something open:** unresolved?\n"
    "- **Q9 — Another:** also open.\n"
    "\n"
    "# Implementation Plan\n"
    "\n"
    "- [x] **M1** done thing\n"
    "- [ ] **M2** pending thing\n"
    "- [X] **M3** done, capital X\n"
    "\n"
    "# Test Strategy\n"
    "\n"
    "- [ ] this checkbox is NOT plan progress\n"
    "\n"
    "# Summary\n"
    "\n"
    "Done.\n";

TEST(SpecModel, ParsesFrontmatter)
{
    SpecInfo s = parseSpec(kDoc, "/proj/specs/spec-workbench.md");
    EXPECT_TRUE(s.hasFrontmatter);
    EXPECT_EQ(s.title, "Spec Workbench");
    EXPECT_EQ(s.status, "reviewed");
    EXPECT_EQ(s.domain, "ide-feature");
    EXPECT_EQ(s.created, "2026-07-19");
    EXPECT_EQ(s.updated, "2026-07-19");
    EXPECT_EQ(s.id, "workbench");
    ASSERT_EQ(s.depends.size(), 2u);
    EXPECT_EQ(s.depends[0], "spec-a");
    EXPECT_EQ(s.depends[1], "spec-b");
    ASSERT_EQ(s.supersedes.size(), 1u);
    EXPECT_EQ(s.supersedes[0], "old-spec");
    EXPECT_EQ(s.path, "/proj/specs/spec-workbench.md");
}

TEST(SpecModel, CountsPlanAndQuestionsPerSection)
{
    SpecInfo s = parseSpec(kDoc);
    EXPECT_EQ(s.planTotal, 3);      // Test Strategy's checkbox excluded
    EXPECT_EQ(s.planDone, 2);       // [x] and [X]
    EXPECT_EQ(s.openQuestions, 2);  // Q8 and Q9; D1 is not a question
}

TEST(SpecModel, NoFrontmatterIsNotAnError)
{
    SpecInfo s = parseSpec("# Objective\n\nJust a plain document.\n");
    EXPECT_FALSE(s.hasFrontmatter);
    EXPECT_TRUE(s.status.empty());
    EXPECT_EQ(s.planTotal, 0);
}

TEST(SpecModel, RefNamePrefersIdThenFilenameStem)
{
    SpecInfo withId = parseSpec(kDoc, "/proj/specs/spec-workbench.md");
    EXPECT_EQ(withId.refName(), "workbench");
    SpecInfo noId = parseSpec("# Hi\n", "/proj/specs/other-spec.md");
    EXPECT_EQ(noId.refName(), "other-spec");
}

TEST(SpecModel, SectionsWithEmptyDetection)
{
    auto sections = specSections(kDoc);
    ASSERT_EQ(sections.size(), 6u);
    EXPECT_EQ(sections[0].title, "Background");
    EXPECT_FALSE(sections[0].empty);
    EXPECT_EQ(sections[1].title, "Objective");
    EXPECT_TRUE(sections[1].empty);
    EXPECT_EQ(sections[5].title, "Summary");
    EXPECT_FALSE(sections[5].empty);
}

TEST(SpecModel, FrontmatterValueReplaceInsertCreate)
{
    // Replace an existing key.
    std::string updated = withFrontmatterValue(kDoc, "status", "implementing");
    SpecInfo s = parseSpec(updated);
    EXPECT_EQ(s.status, "implementing");
    EXPECT_EQ(s.title, "Spec Workbench"); // others untouched
    EXPECT_EQ(updated.size(), std::string(kDoc).size() +
                              std::string("implementing").size() -
                              std::string("reviewed").size());

    // Insert a missing key before the closing delimiter.
    std::string added = withFrontmatterValue(kDoc, "reviewer", "alistair");
    EXPECT_NE(added.find("reviewer: alistair\n---"), std::string::npos);

    // No frontmatter at all: a block is created ahead of the text.
    std::string created = withFrontmatterValue("# Title\n", "status", "draft");
    SpecInfo c = parseSpec(created);
    EXPECT_TRUE(c.hasFrontmatter);
    EXPECT_EQ(c.status, "draft");
    EXPECT_NE(created.find("# Title"), std::string::npos);
}

static SpecInfo makeSpec(const char *name, const char *status,
                         std::vector<std::string> depends = {})
{
    SpecInfo s;
    s.path = std::string("/p/specs/") + name + ".md";
    s.status = status;
    s.depends = std::move(depends);
    return s;
}

TEST(SpecModel, GatePassesWhenReviewedDepsImplementedNoQuestions)
{
    std::vector<SpecInfo> all = {
        makeSpec("dep", "implemented"),
        makeSpec("me", "reviewed", {"dep"}),
    };
    EXPECT_TRUE(specGateBlockers(all[1], all).empty());
}

TEST(SpecModel, GateBlocksOnStatusDepsAndQuestions)
{
    std::vector<SpecInfo> all = {
        makeSpec("dep-draft", "draft"),
        makeSpec("me", "ready", {"dep-draft", "missing-dep"}),
    };
    all[1].openQuestions = 2;
    auto blockers = specGateBlockers(all[1], all);
    ASSERT_EQ(blockers.size(), 4u);
    EXPECT_NE(blockers[0].find("needs review"), std::string::npos);
    EXPECT_NE(blockers[1].find("'dep-draft' is 'draft'"), std::string::npos);
    EXPECT_NE(blockers[2].find("'missing-dep' not found"), std::string::npos);
    EXPECT_NE(blockers[3].find("2 open questions"), std::string::npos);
}

TEST(SpecModel, GateResolvesDependsByIdToo)
{
    SpecInfo dep = makeSpec("some-file", "implemented");
    dep.id = "the-id";
    std::vector<SpecInfo> all = { dep, makeSpec("me", "reviewed", {"the-id"}) };
    EXPECT_TRUE(specGateBlockers(all[1], all).empty());
}

TEST(SpecModel, KebabCase)
{
    EXPECT_EQ(kebabCase("My Great Feature!"), "my-great-feature");
    EXPECT_EQ(kebabCase("  spaced   out  "), "spaced-out");
    EXPECT_EQ(kebabCase("Already-Kebab"), "already-kebab");
    EXPECT_EQ(kebabCase("!!!"), "");
}

TEST(SpecModel, TemplateRoundTrips)
{
    std::string doc = specTemplate("Payment Flow", "saas", "Take payments.",
                                   "2026-07-19");
    SpecInfo s = parseSpec(doc);
    EXPECT_EQ(s.title, "Payment Flow");
    EXPECT_EQ(s.status, "draft");
    EXPECT_EQ(s.domain, "saas");
    EXPECT_EQ(s.created, "2026-07-19");
    auto sections = specSections(doc);
    ASSERT_EQ(sections.size(), 12u);
    EXPECT_EQ(sections[0].title, "Background");
    EXPECT_TRUE(sections[0].empty);
    EXPECT_EQ(sections[1].title, "Objective");
    EXPECT_FALSE(sections[1].empty); // seeded with the goal
    EXPECT_EQ(sections[11].title, "Summary");
}

} // namespace
