/*
 * Program.cs
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.IO;

namespace actorcompiler
{
    class Program
    {
        public static int Main(string[] args)
        {
            if (args.Length < 2)
            {
                Console.WriteLine("Usage:");
                Console.WriteLine("  actorcompiler <input> <output> [--disable-actor-without-wait-warning]");
                return 100;
            }
            Console.WriteLine("actorcompiler {0}", string.Join(" ", args));
            string input = args[0], output = args[1], outputtmp = args[1] + ".tmp";
            ErrorMessagePolicy errorMessagePolicy = new ErrorMessagePolicy();
            if (args.Contains("--disable-actor-without-wait-warning"))
            {
                errorMessagePolicy.DisableActorWithoutWaitWarning = true;
            }
            try
            {
                var inputData = File.ReadAllText(input);
                using (var outputStream = new StreamWriter(outputtmp))
                    new ActorParser(inputData, input.Replace('\\', '/'), errorMessagePolicy).Write(outputStream, output.Replace('\\', '/'));
                if (File.Exists(output))
                {
                    File.SetAttributes(output, FileAttributes.Normal);
                    File.Delete(output);
                }
                File.Move(outputtmp, output);
                File.SetAttributes(output, FileAttributes.ReadOnly);
                return 0;
            }
            catch (actorcompiler.Error e)
            {
                Console.Error.WriteLine("{0}({1}): error FAC1000: {2}", input, e.SourceLine, e.Message);
                if (File.Exists(outputtmp))
                    File.Delete(outputtmp);
                if (File.Exists(output))
                {
                    File.SetAttributes(output, FileAttributes.Normal);
                    File.Delete(output);
                }
                return 1;
            }
            catch (Exception e)
            {
                Console.Error.WriteLine("{0}({1}): error FAC2000: Internal {2}", input, 1, e.ToString());
                if (File.Exists(outputtmp))
                    File.Delete(outputtmp);
                if (File.Exists(output))
                {
                    File.SetAttributes(output, FileAttributes.Normal);
                    File.Delete(output);
                }
                return 3;
            }
        }
    }
}
