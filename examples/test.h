//
// Created by Jingyu Zhou on 2018-12-04.
//

#ifndef FOUNDATIONDB_TEST_H
#define FOUNDATIONDB_TEST_H

#define RUN_TEST(FUNC) do { std::cout <<  "Running " << #FUNC << "...\n"; \
                            FUNC(); \
                            std::cout << std::endl; } while (0)

#endif //FOUNDATIONDB_TEST_H
